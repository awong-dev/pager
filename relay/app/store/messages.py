"""`messages/{id}`, `wireIds/{wireId}_{recipientUid}`, `conversations/{convKey}`
-- docs/SERVER_PLAN.md §3, §5.2.

This module builds the two transactional invariants Phase 3's `routing.py`
depends on:

1. **`wireIds` dedup.** A device up-message with no `to` fans out into one
   `messages/{id}` per recipient sharing the same `wireId`; `create_message`
   stages a `transaction.create()` of `wireIds/{wireId}_{recipientUid}`
   *inside* the same transaction as the message. `create()` raises
   `AlreadyExists` if that document is already there, which is the dedup:
   `create_message` catches it and returns `None` rather than creating a
   second copy of an already-delivered message for that (wireId, recipient)
   pair.
2. **`seqCounter`.** Thread order is `seq` (docs/PROTOCOL.md §3.5's
   insertion-order rule), a value read-then-incremented on
   `settings/meta.seqCounter` inside the same transaction as the message
   create, so two concurrent sends can never be assigned the same `seq` --
   the relay is Firestore's only writer, so this is the one place ordering
   could otherwise race.

Full routing (allow-list checks, fan-out to enabled backends, adapter
delivery) is Phase 3; this module only owns the document shapes and the two
invariants above, plus enough read/update surface for admin and the import
script.
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction
from app.ids import new_id

MessageKind = Literal["text", "loc_req", "loc"]
DeliveryState = Literal[
    "queued", "sent", "shown", "read", "fulfilled", "failed", "expired"
]

PREVIEW_MAX_CHARS = 120


class Delivery(BaseModel):
    model_config = ConfigDict(extra="ignore")

    kind: str
    state: DeliveryState = "queued"
    attempts: int = 0
    externalId: str | None = None
    error: str | None = None
    sentTs: int | None = None
    shownTs: int | None = None
    readTs: int | None = None


class Message(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    seq: int
    convKey: str
    uids: list[str]
    senderUid: str
    recipientUid: str
    kind: MessageKind
    body: str | None = None
    loc: dict | None = None
    wireId: str | None = None
    originBackendId: str | None = None
    ts: int
    createdAt: datetime | None = None
    deliveries: dict[str, Delivery] = {}
    pendingDeviceIds: list[str] = []


class Conversation(BaseModel):
    model_config = ConfigDict(extra="ignore")

    convKey: str
    uids: list[str]
    lastMessageAt: datetime | None = None
    lastPreview: str = ""
    unread: dict[str, int] = {}


def conv_key(uid_a: str, uid_b: str) -> str:
    return "_".join(sorted([uid_a, uid_b]))


def _messages():
    return get_db().collection("messages")


def _wire_ids():
    return get_db().collection("wireIds")


def _conversations():
    return get_db().collection("conversations")


def _meta_ref():
    return get_db().collection("settings").document("meta")


def create_message(
    *,
    sender_uid: str,
    recipient_uid: str,
    kind: MessageKind,
    ts: int,
    body: str | None = None,
    loc: dict | None = None,
    wire_id: str | None = None,
    origin_backend_id: str | None = None,
    deliveries: dict[str, dict] | None = None,
    pending_device_ids: list[str] | None = None,
    msg_id: str | None = None,
    created_at: object = SERVER_TIMESTAMP,
) -> Message | None:
    """One transaction: dedup on `wireId` (if given), increment
    `settings/meta.seqCounter`, create the message, upsert the conversation
    summary. Returns `None` if `wire_id` was already recorded for this
    recipient (dedup fired, no document created).

    `msg_id`/`created_at` are override hooks for `app/db/import_sqlite.py`
    only (it needs to preserve the MVP's original message ids and
    timestamps); routing.py callers should never pass them and get a fresh
    id + `SERVER_TIMESTAMP` by default."""
    msg_id = msg_id or new_id("m_")
    key = conv_key(sender_uid, recipient_uid)
    uids = sorted([sender_uid, recipient_uid])
    msg_ref = _messages().document(msg_id)
    conv_ref = _conversations().document(key)
    meta_ref = _meta_ref()
    wire_ref = _wire_ids().document(f"{wire_id}_{recipient_uid}") if wire_id else None
    preview = (body or "")[:PREVIEW_MAX_CHARS]

    def _txn(transaction: Transaction) -> None:
        # Firestore transactions require every read before any write is
        # staged (the client enforces this locally) -- so both reads happen
        # first, and every write (including the wireId dedup `create()`) is
        # staged afterwards. The dedup guarantee is unaffected: `create()`
        # still fails the whole transaction at commit if the document
        # exists by then, which is what matters.
        meta_snap = meta_ref.get(transaction=transaction)
        conv_snap = conv_ref.get(transaction=transaction)

        if wire_ref is not None:
            # Dedup: raises AlreadyExists (not retried -- that's a real
            # conflict, not transaction contention) if this (wireId,
            # recipient) pair was already delivered.
            transaction.create(wire_ref, {"messageId": msg_id})

        seq = ((meta_snap.get("seqCounter") if meta_snap.exists else 0) or 0) + 1
        if meta_snap.exists:
            transaction.update(meta_ref, {"seqCounter": seq})
        else:
            transaction.set(
                meta_ref, {"schemaVersion": 2, "lastSweepAt": None, "seqCounter": seq}
            )

        transaction.set(
            msg_ref,
            {
                "seq": seq,
                "convKey": key,
                "uids": uids,
                "senderUid": sender_uid,
                "recipientUid": recipient_uid,
                "kind": kind,
                "body": body,
                "loc": loc,
                "wireId": wire_id,
                "originBackendId": origin_backend_id,
                "ts": ts,
                "createdAt": created_at,
                "deliveries": deliveries or {},
                "pendingDeviceIds": pending_device_ids or [],
            },
        )

        unread = dict(conv_snap.get("unread") or {}) if conv_snap.exists else {}
        unread[recipient_uid] = unread.get(recipient_uid, 0) + 1
        conv_data = {
            "uids": uids,
            "lastMessageAt": SERVER_TIMESTAMP,
            "lastPreview": preview,
            "unread": unread,
        }
        if conv_snap.exists:
            transaction.update(conv_ref, conv_data)
        else:
            transaction.set(conv_ref, conv_data)

    try:
        run_transaction(_txn)
    except AlreadyExists:
        return None

    fetched = get_message(msg_id)
    assert fetched is not None
    return fetched


def get_message(msg_id: str) -> Message | None:
    snap = _messages().document(msg_id).get()
    if not snap.exists:
        return None
    return Message.model_validate({"id": msg_id, **(snap.to_dict() or {})})


def list_thread(key: str, *, limit: int = 50) -> list[Message]:
    # `limit_to_last` queries can't be streamed (google-cloud-firestore
    # buffers server-side to reverse them) -- `.get()` instead of `.stream()`.
    query = (
        _messages()
        .where(filter=FieldFilter("convKey", "==", key))
        .order_by("seq")
        .limit_to_last(limit)
    )
    return [
        Message.model_validate({"id": snap.id, **(snap.to_dict() or {})}) for snap in query.get()
    ]


def list_pending_for_device(device_id: str, *, limit: int = 10) -> list[Message]:
    """§5.3's online-edge re-publish query: `pendingDeviceIds array-contains
    deviceId`, oldest first, capped. Consumed by Phase 3's pager backend."""
    query = (
        _messages()
        .where(filter=FieldFilter("pendingDeviceIds", "array_contains", device_id))
        .order_by("createdAt")
        .limit(limit)
    )
    return [
        Message.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]


def update_delivery(msg_id: str, backend_id: str, **fields: object) -> Message:
    """Merge-update of one backend's delivery sub-map. This is the storage
    primitive only -- the full monotonic ack state machine (PROTOCOL.md §4.1:
    idempotent, never-backwards, out-of-order `read` backfills `shown`) is
    Phase 3's `routing.py`/`ingest.py`, mirroring what `app/store.py`'s
    `apply_ack` did for the MVP's SQLite schema."""
    ref = _messages().document(msg_id)
    updates = {f"deliveries.{backend_id}.{k}": v for k, v in fields.items()}
    if updates:
        ref.update(updates)
    fetched = get_message(msg_id)
    if fetched is None:
        raise KeyError(f"no such message: {msg_id!r}")
    return fetched


def set_pending_device_ids(msg_id: str, device_ids: list[str]) -> None:
    _messages().document(msg_id).update({"pendingDeviceIds": device_ids})


def get_conversation(key: str) -> Conversation | None:
    snap = _conversations().document(key).get()
    if not snap.exists:
        return None
    return Conversation.model_validate({"convKey": key, **(snap.to_dict() or {})})

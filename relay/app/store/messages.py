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

import time
from datetime import UTC, datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import (
    SERVER_TIMESTAMP,
    ArrayRemove,
    FieldFilter,
    Transaction,
)
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction
from app.ids import new_id

MessageKind = Literal["text", "loc_req", "loc"]
DeliveryState = Literal[
    "queued", "sent", "shown", "read", "fulfilled", "failed", "expired"
]
AckResult = Literal["updated", "noop", "unknown"]

PREVIEW_MAX_CHARS = 120
# docs/SERVER_PLAN.md §5.2: "Any failure leaves the delivery 'queued'/
# 'failed' with attempts+1 ... max 5 -> failed".
MAX_DELIVERY_ATTEMPTS = 5
# PROTOCOL.md §4: a down message still 'queued'/'sent' 24h after creation is
# `expired` (derived at read time -- the same rule `app/store/legacy.py`
# applies). Used here only to bound the online-edge republish / tick
# candidate queries so a years-old unacked message can't be republished
# forever; it does not itself write an 'expired' state anywhere.
EXPIRY_SECONDS = 24 * 60 * 60


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
    originBackendKind: str | None = None
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
    origin_backend_kind: str | None = None,
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
            # recipient) pair was already delivered. `createdAt` (Phase 6
            # M1 fix, build review) lets `app/jobs.py`'s `sweep()` reclaim
            # this doc on its own schedule rather than only ever as a
            # side effect of its parent message's delete -- once a future
            # user-deletion pass (§5.7: "deleting a user removes every
            # document keyed by their UID") deletes the message directly,
            # this doc would otherwise be orphaned forever (it carries no
            # other path back to a `messages` doc once that doc is gone).
            transaction.create(wire_ref, {"messageId": msg_id, "createdAt": SERVER_TIMESTAMP})

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
                "originBackendKind": origin_backend_kind,
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


def messages_ref(msg_id: str):
    """Public (unlike `_messages`) because `app/location.py` builds its own
    multi-document Firestore transactions on top of this collection's shape
    (docs/PROTOCOL.md §13.4's `loc_req` fulfilment -- one transaction across
    `locReqs/{deviceId}`, the `loc_req` message and one `kind='loc'` message
    per coalesced requester -- can't be built out of `create_message`, which
    opens its own separate transaction and Firestore transactions cannot
    nest)."""
    return _messages().document(msg_id)


def conversation_ref(key: str):
    """See `messages_ref`'s docstring -- same reason."""
    return _conversations().document(key)


def meta_ref():
    """See `messages_ref`'s docstring -- same reason."""
    return _meta_ref()


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


def list_pending_for_device(
    device_id: str, *, limit: int = 10, max_age_s: int | None = EXPIRY_SECONDS
) -> list[Message]:
    """§5.3's online-edge re-publish query -- and, filtered further by the
    caller to `state == 'queued'` deliveries only, `/internal/tick`'s retry
    query (§5.8 item 1): `pendingDeviceIds array-contains deviceId`, oldest
    first, capped. `max_age_s` (default 24h, PROTOCOL.md §4) bounds it to
    non-expired candidates the same way `app/store/legacy.py`'s republish
    query does; pass `None` to disable the bound."""
    query = _messages().where(filter=FieldFilter("pendingDeviceIds", "array_contains", device_id))
    if max_age_s is not None:
        cutoff = datetime.fromtimestamp(time.time() - max_age_s, tz=UTC)
        query = query.where(filter=FieldFilter("createdAt", ">", cutoff))
    query = query.order_by("createdAt").limit(limit)
    return [
        Message.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]


def list_recent_queued_by_kind(
    kind: str, *, limit: int = 50, max_age_s: int = EXPIRY_SECONDS
) -> list[Message]:
    """`(build addition, phase 5)`: every message created within
    `max_age_s` (default 24h, same bound `list_pending_for_device` uses)
    with at least one `kind`-backend delivery still `queued`, *oldest*
    `limit` candidates by `createdAt` (ascending -- the same "oldest first"
    ordering docs/SERVER_PLAN.md §5.8 item 1 specifies for the pager
    retry). `(review note, phase 5)` the `kind`/`queued` filter runs in
    Python *after* the `limit`, so a deployment producing more than `limit`
    messages inside `max_age_s` can leave newer queued deliveries unvisited
    until the older candidates drain (they drain fast: `attempts` reaches
    `MAX_DELIVERY_ATTEMPTS` -> `failed` within a few ticks). Acceptable at
    this project's volume (§9.3); the real fix is Phase 7/8's
    enqueue-at-send-time Cloud Tasks retry, which needs no scan at all.
    Unlike `list_pending_for_device`
    (which queries the indexed `pendingDeviceIds array-contains` field),
    there is no equivalent indexed array field for "some backend of kind X
    is still queued" for a generic, non-`pager` kind -- `pendingDeviceIds`
    is deliberately device-id-keyed, per `app/routing.py`'s "device default"
    handling, and adding a parallel generic array field is a real schema
    change docs/SERVER_PLAN.md does not call for. So this filters in Python
    over a bounded, already-indexed `createdAt` scan (`messages(createdAt)`,
    the same index the retention sweep uses) instead -- correct, and cheap
    at this project's household message volume (§9.3), if not as tight as a
    purpose-built index. Used by `app/jobs.py`'s `tick()` to retry queued
    non-pager deliveries (today just `sms`, `app/backends/sms_stub.py`)."""
    cutoff = datetime.fromtimestamp(time.time() - max_age_s, tz=UTC)
    query = (
        _messages()
        .where(filter=FieldFilter("createdAt", ">", cutoff))
        .order_by("createdAt")
        .limit(limit)
    )
    out = []
    for snap in query.stream():
        msg = Message.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        if any(d.kind == kind and d.state == "queued" for d in msg.deliveries.values()):
            out.append(msg)
    return out


def clear_pending_device(msg_id: str, device_id: str) -> None:
    """Removes `device_id` from `pendingDeviceIds` -- called once its pager
    delivery reaches a non-pending state (shown/read/failed), so the
    online-edge republish and `/internal/tick` stop selecting it."""
    _messages().document(msg_id).update({"pendingDeviceIds": ArrayRemove([device_id])})


def find_pager_delivery(msg: Message, device_id: str) -> str | None:
    """The backend id (`bid`) of `msg`'s `pager`-kind delivery addressed to
    `device_id` -- `PagerBackend.deliver()` stashes the device id in
    `externalId` (docs/SERVER_PLAN.md §6.1's `Delivery.externalId`, reused
    rather than adding a pager-only field) precisely so an inbound ack can be
    matched back to the right delivery entry without a second Firestore read
    of the backend doc. `None` if `msg` has no delivery for this device
    (§4.1 rule 4's "wrong device" case)."""
    for bid, delivery in msg.deliveries.items():
        if delivery.kind == "pager" and delivery.externalId == device_id:
            return bid
    return None


def find_delivery_by_kind(msg: Message, kind: str) -> str | None:
    """The first delivery of `kind` in `msg.deliveries` -- since one message
    document belongs to exactly one (senderUid, recipientUid) pair, every
    delivery in it was fanned out for the same `recipientUid`, so there is
    at most one delivery per kind in practice (a recipient with two enabled
    backends of the same kind is not a shape `app/routing.py` produces).
    Used by `app/routers/conversations.py`'s read-receipt endpoint to find
    the recipient's `webapp` delivery."""
    for bid, delivery in msg.deliveries.items():
        if delivery.kind == kind:
            return bid
    return None


def mark_delivery_sent_if_queued(msg_id: str, backend_id: str) -> None:
    """`queued` -> `sent`, and *only* `queued` -> `sent` -- transactional
    read-then-write so a concurrent ack landing between the two can never be
    walked backwards (PROTOCOL.md §4.1 rule 2), mirroring
    `app/store/legacy.py`'s `mark_sent`."""
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> None:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            return
        delivery = ((snap.to_dict() or {}).get("deliveries") or {}).get(backend_id)
        if delivery and delivery.get("state") == "queued":
            transaction.update(ref, {f"deliveries.{backend_id}.state": "sent"})

    run_transaction(_txn)


def apply_delivery_ack(
    msg_id: str,
    backend_id: str,
    device_id: str | None,
    ack: Literal["shown", "read"],
    ack_ts: int,
    *,
    clear_unread_uid: str | None = None,
) -> AckResult:
    """The monotonic ack state machine (PROTOCOL.md §4.1 rules 1-3), scoped
    to one backend's entry in `deliveries` rather than a whole legacy row:
    idempotent on a state already reached, `read` without a prior `shown`
    back-fills `shownTs`, and (on either transition, when `device_id` is
    given) clears it from `pendingDeviceIds` in the same transaction since
    the delivery is no longer pending once acked. `device_id` is only
    meaningful for `pager` deliveries (`webapp`'s read-receipt caller,
    `app/routers/conversations.py`, passes `None` -- webapp deliveries never
    populate `pendingDeviceIds` in the first place).

    `clear_unread_uid` (S3a `build finding`): when given and this call
    actually applies a `read` transition (not a `shown` ack, and not a
    no-op on a message already `read`), zeroes
    `conversations/{convKey}.unread[clear_unread_uid]` in the *same*
    transaction as the ack. Without this, `unread` -- incremented on every
    `create_message` (see that function) -- only ever grew, since nothing
    cleared it when the recipient actually read a thread.
    `app/routers/conversations.py`'s `mark_read` is the only caller that
    passes it (the webapp read-receipt path); `app/ingest.py`'s pager-ack
    caller leaves it `None`, matching this phase's brief scoping the fix to
    the webapp mark-read handler."""
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> AckResult:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            return "unknown"
        data = snap.to_dict() or {}
        delivery = (data.get("deliveries") or {}).get(backend_id)
        if delivery is None:
            return "unknown"
        state = delivery.get("state")
        updates: dict[str, object] = {}
        if ack == "shown":
            if state in ("shown", "read"):
                return "noop"
            updates[f"deliveries.{backend_id}.state"] = "shown"
            updates[f"deliveries.{backend_id}.shownTs"] = ack_ts
        elif ack == "read":
            if state == "read":
                return "noop"
            shown_ts = delivery.get("shownTs") if delivery.get("shownTs") is not None else ack_ts
            updates[f"deliveries.{backend_id}.state"] = "read"
            updates[f"deliveries.{backend_id}.readTs"] = ack_ts
            updates[f"deliveries.{backend_id}.shownTs"] = shown_ts
        else:
            return "unknown"
        if device_id is not None:
            updates["pendingDeviceIds"] = ArrayRemove([device_id])

        # Every read this transaction needs (Firestore requires all reads
        # before any write is staged) happens here, before the
        # `transaction.update(ref, ...)` below.
        conv_ref = None
        conv_snap = None
        if clear_unread_uid is not None and ack == "read":
            conv_key = data.get("convKey")
            if conv_key:
                conv_ref = _conversations().document(conv_key)
                conv_snap = conv_ref.get(transaction=transaction)

        transaction.update(ref, updates)
        if conv_ref is not None and conv_snap is not None and conv_snap.exists:
            unread = dict(conv_snap.get("unread") or {})
            unread[clear_unread_uid] = 0
            transaction.update(conv_ref, {"unread": unread})
        return "updated"

    return run_transaction(_txn)


def record_delivery_attempt(
    msg_id: str,
    backend_id: str,
    *,
    ok: bool,
    error: str | None,
    device_id: str | None = None,
    max_attempts: int = MAX_DELIVERY_ATTEMPTS,
) -> None:
    """Applied once per `app/backends/*` `deliver()` call, *after* the
    backend's own delivery-state transaction (e.g. `pager`'s call to
    `mark_delivery_sent_if_queued` on a 2xx publish) has already run --
    docs/SERVER_PLAN.md §5.2: "Any failure leaves the delivery
    'queued'/'failed' with attempts+1 ... max 5 -> failed". One transaction:
    increments `attempts` and records `error` (`None` clears a stale one on
    an eventual success). If a backend's own transition never reached a
    success state (i.e. `deliveries.{bid}.state` is still `queued` or
    already `failed` -- `sent`/`shown`/`read`/`fulfilled` are all states
    only a successful `deliver()` call reaches) and this call's `ok=False`
    brings `attempts` to `max_attempts`, marks the delivery `failed` and, if
    `device_id` is given (a `pager` delivery -- see `find_pager_delivery`'s
    docstring for why `externalId` doubles as the device id), removes it
    from `pendingDeviceIds` in the same transaction. Without this, a
    delivery that can never succeed (e.g. permanently broken credentials)
    would stay in `pendingDeviceIds` forever, retried by every tick and
    every online-edge event for the rest of PROTOCOL.md §4's 24h window."""
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> None:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            return
        data = snap.to_dict() or {}
        delivery = (data.get("deliveries") or {}).get(backend_id)
        if delivery is None:
            return
        attempts = (delivery.get("attempts") or 0) + 1
        updates: dict[str, object] = {
            f"deliveries.{backend_id}.attempts": attempts,
            f"deliveries.{backend_id}.error": error,
        }
        state = delivery.get("state")
        reached_success = state not in ("queued", "failed")
        if not ok and not reached_success and attempts >= max_attempts:
            updates[f"deliveries.{backend_id}.state"] = "failed"
            if device_id is not None:
                updates["pendingDeviceIds"] = ArrayRemove([device_id])
        transaction.update(ref, updates)

    run_transaction(_txn)


def set_pending_device_ids(msg_id: str, device_ids: list[str]) -> None:
    _messages().document(msg_id).update({"pendingDeviceIds": device_ids})


def get_conversation(key: str) -> Conversation | None:
    snap = _conversations().document(key).get()
    if not snap.exists:
        return None
    return Conversation.model_validate({"convKey": key, **(snap.to_dict() or {})})

"""Firestore-backed replacement for the MVP's SQLite `app/store.py`, used
only by the legacy `RELAY_TOKEN` endpoints (`POST/GET /api/devices/{id}/
messages`, deleted in Phase 6 per docs/SERVER_PLAN.md §5.1) and by
`app/ingest.py`'s device-status/ack handling until Phase 3's `routing.py`
replaces both with the uid-addressed `messages/{id}` model.

**Not part of docs/SERVER_PLAN.md §3's schema.** That schema is keyed on
(senderUid, recipientUid) pairs behind an allow-list, which the legacy
per-device MVP thread has no equivalent of (there is no "user" on either
end, just a device and an implicit "parent"). Rather than force the legacy
shape into the v2 uid model -- which would mean synthesizing fake users and
allow-list edges just to keep an endpoint alive that Phase 6 deletes anyway
-- this module keeps the legacy thread in its own collections, structured
exactly like the old SQLite schema (`legacyMessages`, `legacyStatus`) so its
*behavior* (ack state machine, republish selection, dedup) is unchanged, only
its storage engine is. `import_sqlite.py` migrates old data into the real
`messages/{id}` shape instead of this one, because that import produces
`parent`/`student` *users*, at which point the v2 model applies.

Collections (outside §3, flagged for the orchestrator):
    legacyMessages/{id}       {id, deviceId, direction, v, ts, sender, body,
                                state, shownTs, readTs, createdAt, seq}
    legacyStatus/{deviceId}   {state, mode, battMv, rssi, session, ts, fw, updatedAt}
    legacySettings/meta       {seqCounter}   -- ordering counter, same
                                                 transactional pattern as
                                                 settings/meta.seqCounter
"""

from __future__ import annotations

import time
from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction
from app.ids import new_id

EXPIRY_SECONDS = 24 * 60 * 60
REPUBLISH_CAP = 10

AckResult = Literal["updated", "noop", "unknown"]


class LegacyMessage(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    deviceId: str
    direction: Literal["down", "up"]
    v: int = 1
    ts: int
    sender: str | None = None
    body: str | None = None
    state: str
    shownTs: int | None = None
    readTs: int | None = None
    createdAt: int
    seq: int

    def effective_state(self, now: int | None = None) -> str:
        if self.direction == "up":
            return "received"
        now = now if now is not None else int(time.time())
        if self.state in ("queued", "sent") and (now - self.createdAt) >= EXPIRY_SECONDS:
            return "expired"
        return self.state


class LegacyStatus(BaseModel):
    model_config = ConfigDict(extra="ignore")

    deviceId: str
    state: str
    mode: str | None = None
    battMv: int | None = None
    rssi: int | None = None
    session: str | None = None
    ts: int | None = None
    fw: str | None = None
    updatedAt: datetime | None = None


def _messages():
    return get_db().collection("legacyMessages")


def _status():
    return get_db().collection("legacyStatus")


def _meta_ref():
    return get_db().collection("legacySettings").document("meta")


def _next_seq(transaction: Transaction) -> int:
    meta_ref = _meta_ref()
    snap = meta_ref.get(transaction=transaction)
    seq = ((snap.get("seqCounter") if snap.exists else 0) or 0) + 1
    if snap.exists:
        transaction.update(meta_ref, {"seqCounter": seq})
    else:
        transaction.set(meta_ref, {"seqCounter": seq})
    return seq


def create_down_message(
    *, msg_id: str, device_id: str, ts: int, body: str, now: int | None = None
) -> LegacyMessage:
    now = now if now is not None else int(time.time())
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> None:
        seq = _next_seq(transaction)
        transaction.set(
            ref,
            {
                "deviceId": device_id,
                "direction": "down",
                "v": 1,
                "ts": ts,
                "sender": "parent",
                "body": body,
                "state": "queued",
                "shownTs": None,
                "readTs": None,
                "createdAt": now,
                "seq": seq,
            },
        )

    run_transaction(_txn)
    fetched = get_message(msg_id)
    assert fetched is not None
    return fetched


def insert_up_message(
    *,
    msg_id: str,
    device_id: str,
    ts: int,
    sender: str | None,
    body: str | None,
    now: int | None = None,
) -> bool:
    """Idempotent insert -- returns False (no-op) on a duplicate `id`
    (PROTOCOL.md §4.1 rule 1 / §4.2). Uses `transaction.create()` (not a
    pre-check + insert) so two concurrent at-least-once webhook deliveries
    of the same up message can't both race past an existence check, mirroring
    the old SQLite `INSERT ... ON CONFLICT DO NOTHING` guarantee."""
    now = now if now is not None else int(time.time())
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> None:
        seq = _next_seq(transaction)
        transaction.create(
            ref,
            {
                "deviceId": device_id,
                "direction": "up",
                "v": 1,
                "ts": ts,
                "sender": sender,
                "body": body,
                "state": "received",
                "shownTs": None,
                "readTs": None,
                "createdAt": now,
                "seq": seq,
            },
        )

    try:
        run_transaction(_txn)
    except AlreadyExists:
        return False
    return True


def mark_sent(msg_id: str) -> None:
    """queued -> sent, and *only* queued -> sent. Transactional because the
    MVP's SQLite version was one conditional statement (`UPDATE ... WHERE
    id = ? AND state = 'queued'`); a read-then-write here would let an ack
    that lands between the two steps be overwritten, walking the state
    backwards from shown/read to sent (PROTOCOL.md §4.1's never-backwards
    rule)."""
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> None:
        snap = ref.get(transaction=transaction)
        if snap.exists and (snap.to_dict() or {}).get("state") == "queued":
            transaction.update(ref, {"state": "sent"})

    run_transaction(_txn)


def apply_ack(msg_id: str, ack: str, ack_ts: int) -> AckResult:
    """Same monotonic ack rules as the MVP's `app/store.py` (PROTOCOL.md
    §4.1): idempotent on a state already reached, `read` without a prior
    `shown` back-fills `shownTs`."""
    ref = _messages().document(msg_id)

    def _txn(transaction: Transaction) -> AckResult:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            return "unknown"
        data = snap.to_dict() or {}
        state = data.get("state")
        if ack == "shown":
            if state in ("shown", "read"):
                return "noop"
            transaction.update(ref, {"state": "shown", "shownTs": ack_ts})
            return "updated"
        if ack == "read":
            if state == "read":
                return "noop"
            shown_ts = data.get("shownTs") if data.get("shownTs") is not None else ack_ts
            transaction.update(ref, {"state": "read", "readTs": ack_ts, "shownTs": shown_ts})
            return "updated"
        return "unknown"

    return run_transaction(_txn)


def id_exists(msg_id: str) -> bool:
    return _messages().document(msg_id).get().exists


def get_message(msg_id: str) -> LegacyMessage | None:
    snap = _messages().document(msg_id).get()
    if not snap.exists:
        return None
    return LegacyMessage.model_validate({"id": msg_id, **(snap.to_dict() or {})})


def get_thread(device_id: str, *, since: int | None = None) -> list[LegacyMessage]:
    query = _messages().where(filter=FieldFilter("deviceId", "==", device_id))
    if since is not None:
        query = query.where(filter=FieldFilter("createdAt", ">", since))
    query = query.order_by("seq")
    return [
        LegacyMessage.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]


def get_republish_candidates(
    device_id: str,
    *,
    now: int | None = None,
    max_age: int = EXPIRY_SECONDS,
    limit: int = REPUBLISH_CAP,
) -> list[LegacyMessage]:
    now = now if now is not None else int(time.time())
    cutoff = now - max_age
    query = (
        _messages()
        .where(filter=FieldFilter("deviceId", "==", device_id))
        .where(filter=FieldFilter("direction", "==", "down"))
        .where(filter=FieldFilter("state", "in", ["queued", "sent"]))
        .where(filter=FieldFilter("createdAt", ">", cutoff))
        .order_by("seq")
        .limit(limit)
    )
    return [
        LegacyMessage.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]


def get_queued_down_messages(
    device_id: str,
    *,
    now: int | None = None,
    max_age: int = EXPIRY_SECONDS,
    limit: int = REPUBLISH_CAP,
) -> list[LegacyMessage]:
    now = now if now is not None else int(time.time())
    cutoff = now - max_age
    query = (
        _messages()
        .where(filter=FieldFilter("deviceId", "==", device_id))
        .where(filter=FieldFilter("direction", "==", "down"))
        .where(filter=FieldFilter("state", "==", "queued"))
        .where(filter=FieldFilter("createdAt", ">", cutoff))
        .order_by("seq")
        .limit(limit)
    )
    return [
        LegacyMessage.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]


def get_status(device_id: str) -> LegacyStatus | None:
    snap = _status().document(device_id).get()
    if not snap.exists:
        return None
    return LegacyStatus.model_validate({"deviceId": device_id, **(snap.to_dict() or {})})


def upsert_status(
    device_id: str,
    *,
    state: str,
    mode: str | None,
    batt_mv: int | None,
    rssi: int | None,
    session: str | None,
    ts: int | None,
    fw: str | None,
) -> None:
    _status().document(device_id).set(
        {
            "state": state,
            "mode": mode,
            "battMv": batt_mv,
            "rssi": rssi,
            "session": session,
            "ts": ts,
            "fw": fw,
            "updatedAt": SERVER_TIMESTAMP,
        }
    )


def new_legacy_message_id() -> str:
    return new_id("m_")

"""`devices/{deviceId}/smsLog/{logId}` -- docs/V02_DESIGN.md §6/§7 (device-
direct SMS audit log).

The pager's own modem sends/receives SMS directly to/from a parent-managed
allow-list, without the relay in the loop for delivery -- the relay's only
role is (a) the place the parent manages that allow-list
(`devices/{id}.smsContacts`, `app/store/devices.py`) and (b) this audit log.
`sms_log` is therefore **not** a thread entry (no `messages/{id}` row, no
`conversations/{convKey}` update, no delivery/backend fan-out) and is
**not** routed to any user -- it is written here, once, keyed by the
envelope's own `id` so a broker webhook redelivery (docs/PROTOCOL.md §2's
"the push is at-least-once") is idempotent: a repeat of the same `sms_log`
`id` is a silent no-op, never a second row.

Same collection-primitive shape as `app/store/locations.py`
(`devices/{deviceId}/locations/{autoId}`), except keyed by the device's own
chosen `id` rather than an auto-id -- the dedup key *is* the document id,
the same idiom `app/store/contacts.py`'s `contactRequests/{deviceId}_
{reqId}` and `app/location.py`'s `locWireIds/{id}` use elsewhere in this
codebase for a device-chosen id.

Ingest logic (signature/replay verification, shape validation, the
`SECURITY sms-blocked` log line) lives in `app/ingest.py`; this module is
just the collection primitive plus the read support `GET
/api/devices/{id}/sms-log` needs (`app/routers/devices.py`).
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

Direction = Literal["out", "in"]
Status = Literal["sent", "failed", "recv", "blocked"]


class SmsLogEntry(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    ts: int
    smsTs: int
    dir: Direction
    peer: str
    st: Status
    body: str = ""
    receivedAt: datetime | None = None


def _sms_log(device_id: str):
    return get_db().collection("devices").document(device_id).collection("smsLog")


def create_log(
    device_id: str,
    log_id: str,
    *,
    ts: int,
    sms_ts: int,
    dir_: Direction,
    peer: str,
    st: Status,
    body: str,
) -> bool:
    """Writes `devices/{device_id}/smsLog/{log_id}`. Returns `True` if this
    call created the row, `False` if `log_id` was already stored (a no-op,
    not an error -- the caller, `app/ingest.py`'s `_handle_sms_log`, still
    logs one INFO line either way per docs/V02_DESIGN.md §6's audit
    requirement; this return value only distinguishes "first time" from
    "redelivered" for that log line).

    `DocumentReference.create()` (not `.set()`) so a redelivered webhook can
    never silently overwrite `receivedAt` with a later ingest time -- the
    same `transaction.create()`-or-`AlreadyExists` idiom `app/location.py`
    and `app/store/messages.py` use for their own id-keyed dedup, just
    without a surrounding transaction: nothing else needs to commit
    atomically with this one write."""
    ref = _sms_log(device_id).document(log_id)
    try:
        ref.create(
            {
                "ts": ts,
                "smsTs": sms_ts,
                "dir": dir_,
                "peer": peer,
                "st": st,
                "body": body,
                "receivedAt": SERVER_TIMESTAMP,
            }
        )
        return True
    except AlreadyExists:
        return False


def list_log(device_id: str, *, limit: int = 100, before: int | None = None) -> list[SmsLogEntry]:
    """Newest first (by the envelope's own `ts`, not `receivedAt` -- `ts` is
    what `before=<epoch seconds>` pagination is keyed against, per
    `GET /api/devices/{id}/sms-log`'s contract). A single-field range filter
    ordered by that same field (`ts`) needs no composite index -- Firestore
    auto-indexes every field ascending/descending for exactly this shape of
    query, so `firestore.indexes.json` needs no new entry for it (unlike
    `app/store/locations.py`'s `createdAt` field override, which exists
    only because `app/jobs.py`'s retention sweep runs a `COLLECTION_GROUP`
    query across every device's `locations` -- this module has no sweep)."""
    query = _sms_log(device_id)
    if before is not None:
        query = query.where(filter=FieldFilter("ts", "<", before))
    query = query.order_by("ts", direction="DESCENDING").limit(limit)
    return [
        SmsLogEntry.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]

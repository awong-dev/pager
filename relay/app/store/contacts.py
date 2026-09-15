"""`contactRequests/{deviceId}_{reqId}` -- docs/DEVICE_PLAN.md §4.1-4.3,
docs/PROTOCOL.md §3.2's `kind:"contact_req"`/`kind:"book"`.

The device's address book is a *projection* of the relay's allow-list plus
the device owner's own pending requests (§4.1): the device can only *ask*
(`/up kind:"contact_req"`, handled by `app.ingest.Ingest`), and only an admin
can approve or reject (`app/routers/admin.py`'s `/api/admin/contacts/*`,
§4.3). This module owns the `contactRequests` collection's CRUD and the two
small pieces of cross-cutting bookkeeping every approve/reject must do:
bumping `devices/{d}.bookVersion` and (eventually) pushing a fresh `book`
down to the device.

**`bump_book_version` is a deliberate, documented exception** to this
codebase's usual "each store module only touches its own collection"
convention (compare `app/store/allow.py`, which calls the *public*
`app/store/devices.py` API rather than writing `devices/{d}` directly).
`devices.py` is not in task S4.1's `Files` list (`docs/DEVICE_TASKS.md`), so
`bookVersion` -- a field `docs/DEVICE_PLAN.md` §4.1 adds to `devices/{d}` --
is written here via the shared `app.db.firestore.get_db()` client instead of
a new `devices_store.bump_book_version`. `devices.py`'s own `Device` pydantic
model does not (yet) declare `bookVersion`, so `devices_store.get_device()`
silently drops it on read (`extra="ignore"`) until a task that has
`devices.py` in scope adds the field there -- the same "write a field the
model doesn't declare yet" shape `devices.py`'s own `record_sig_failure`
already uses for `authFailureTimes`. Flagged here (and in this task's final
report) rather than silently worked around.

**`push_book` is a placeholder for S4.2**, per `docs/DEVICE_TASKS.md`'s own
note that S4.2 (not yet implemented) owns building and signing the `/down`
`book` envelope. `devcfg.py` (S4.2's module) is also outside this task's
`Files` list, so this is intentionally not a real implementation -- every
call site in this file and in `app/routers/admin.py` is exactly where S4.2
should hook in a real publish.
"""

from __future__ import annotations

import logging
from datetime import datetime
from typing import Literal

from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction

logger = logging.getLogger("relay.contacts")

# docs/PROTOCOL.md §3.2: "rate-limited: at most 5 pending requests per device."
MAX_PENDING_PER_DEVICE = 5

Status = Literal["pending", "approved", "rejected"]


class ContactRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    key: str  # "{deviceId}_{reqId}" -- the Firestore document id.
    deviceId: str
    reqId: str
    ownerUid: str
    name: str
    phone: str | None = None
    alias: str | None = None
    status: Status = "pending"
    reason: str | None = None
    createdAt: datetime | None = None
    decidedAt: datetime | None = None
    decidedBy: str | None = None


class TooManyPending(Exception):
    """Raised by `create_request` when `device_id` already has
    `MAX_PENDING_PER_DEVICE` pending requests -- the caller
    (`app.ingest.Ingest._handle_contact_req`) turns this into the one
    `system` down reply `docs/DEVICE_TASKS.md` S4.1 specifies verbatim:
    "too many pending requests"."""


def key(device_id: str, req_id: str) -> str:
    return f"{device_id}_{req_id}"


def _contact_requests():
    return get_db().collection("contactRequests")


def _decode(doc_key: str, data: dict) -> ContactRequest:
    return ContactRequest.model_validate({"key": doc_key, **data})


def get_request(doc_key: str) -> ContactRequest | None:
    snap = _contact_requests().document(doc_key).get()
    if not snap.exists:
        return None
    return _decode(doc_key, snap.to_dict() or {})


def get_by_device_and_req(device_id: str, req_id: str) -> ContactRequest | None:
    return get_request(key(device_id, req_id))


def list_requests(
    *, status: Status | None = None, device_id: str | None = None
) -> list[ContactRequest]:
    query = _contact_requests()
    if device_id is not None:
        query = query.where(filter=FieldFilter("deviceId", "==", device_id))
    if status is not None:
        query = query.where(filter=FieldFilter("status", "==", status))
    return [_decode(snap.id, snap.to_dict() or {}) for snap in query.stream()]


def count_pending(device_id: str) -> int:
    return len(list_requests(status="pending", device_id=device_id))


def has_matching_pending_or_approved(
    device_id: str, *, phone: str | None, alias: str | None
) -> bool:
    """docs/PROTOCOL.md §3.2: "A request whose `ph` or implied alias matches
    an existing pending/approved contact is a no-op." Scoped to this device
    (a device's book only ever lists *its own* pending/approved contacts,
    §4.3), not every contact request across every device."""
    if phone is None and alias is None:
        return False
    for existing in list_requests(device_id=device_id):
        if existing.status not in ("pending", "approved"):
            continue
        if phone is not None and existing.phone == phone:
            return True
        if alias is not None and existing.alias == alias:
            return True
    return False


def create_request(
    *,
    device_id: str,
    owner_uid: str,
    req_id: str,
    name: str,
    phone: str | None = None,
    alias: str | None = None,
) -> ContactRequest | None:
    """Idempotent on `req_id` (dedup by `id`, per §3.2/§4.2: any up message,
    contact_req included, dedups on `id`) -- a redelivered webhook for the
    same request simply returns the already-stored doc rather than erroring
    or double-counting against the pending cap.

    Returns `None` (a no-op, not an error) if `phone`/`alias` already matches
    an existing pending/approved request for this device.

    Raises `TooManyPending` if this device already has
    `MAX_PENDING_PER_DEVICE` pending requests -- checked (and, being a
    docsize count-then-create, subject to a benign race under concurrent
    webhook retries for *different* `req_id`s, which is fine: worst case one
    extra pending request slips in, corrected by the next admin decision;
    the cap is an abuse/UI-clutter guard, not a security boundary)."""
    doc_key = key(device_id, req_id)
    existing = get_request(doc_key)
    if existing is not None:
        return existing

    if has_matching_pending_or_approved(device_id, phone=phone, alias=alias):
        return None

    if count_pending(device_id) >= MAX_PENDING_PER_DEVICE:
        raise TooManyPending(device_id)

    _contact_requests().document(doc_key).set(
        {
            "deviceId": device_id,
            "reqId": req_id,
            "ownerUid": owner_uid,
            "name": name,
            "phone": phone,
            "alias": alias,
            "status": "pending",
            "reason": None,
            "createdAt": SERVER_TIMESTAMP,
            "decidedAt": None,
            "decidedBy": None,
        }
    )
    fetched = get_request(doc_key)
    assert fetched is not None
    return fetched


def approve(doc_key: str, *, decided_by: str) -> ContactRequest:
    ref = _contact_requests().document(doc_key)
    if not ref.get().exists:
        raise KeyError(f"no such contact request: {doc_key!r}")
    ref.update({"status": "approved", "decidedAt": SERVER_TIMESTAMP, "decidedBy": decided_by})
    fetched = get_request(doc_key)
    assert fetched is not None
    return fetched


def reject(doc_key: str, *, reason: str, decided_by: str) -> ContactRequest:
    ref = _contact_requests().document(doc_key)
    if not ref.get().exists:
        raise KeyError(f"no such contact request: {doc_key!r}")
    ref.update(
        {
            "status": "rejected",
            "reason": reason,
            "decidedAt": SERVER_TIMESTAMP,
            "decidedBy": decided_by,
        }
    )
    fetched = get_request(doc_key)
    assert fetched is not None
    return fetched


def bump_book_version(device_id: str) -> int:
    """docs/DEVICE_PLAN.md §4.1: "`devices/{d}.bookVersion` int, bumped by
    every change that alters the device's projection." See this module's
    docstring for why this writes `devices/{device_id}` directly instead of
    going through `app/store/devices.py`."""
    ref = get_db().collection("devices").document(device_id)

    def _txn(transaction: Transaction) -> int:
        snap = ref.get(transaction=transaction)
        data = (snap.to_dict() or {}) if snap.exists else {}
        bv = int(data.get("bookVersion", 0)) + 1
        if snap.exists:
            transaction.update(ref, {"bookVersion": bv})
        else:
            transaction.set(ref, {"bookVersion": bv}, merge=True)
        return bv

    return run_transaction(_txn)


def push_book(device_id: str) -> None:
    """**Placeholder for S4.2** (`docs/DEVICE_TASKS.md` S4.2's `push_book`,
    not yet implemented). Every approve/reject is supposed to publish a
    fresh signed `/down` `kind:"book"` envelope (`docs/DEVICE_PLAN.md` §4.3);
    until S4.2 lands, this is a best-effort no-op that only logs, so the gap
    is visible rather than silently absent. S4.2 should replace this
    function's body (or introduce a real `app/devcfg.py` and have this
    module call that instead) -- every caller in `app/routers/admin.py`
    already calls this function at exactly the point S4.2 needs to publish
    from."""
    logger.info("push_book(%s): not yet implemented -- TODO(orchestrator): S4.2", device_id)

"""`/api/devices/*` -- docs/V02_DESIGN.md §6 (device-direct SMS): the
owner-facing (not admin-only) device surface.

`GET /api/devices` did not exist anywhere before this task (checked: the
only prior device listing is `app/routers/admin.py`'s admin-only
`GET /api/devices` under the `/api/admin` prefix) -- a parent needs some way
to learn their own device's id from the web app without going through the
admin API, so it lives here.

`sms-contacts`/`sms-log` are gated on **owner or admin**
(`_require_owner_or_admin` below), unlike every route in
`app/routers/admin.py`, which is admin-only end to end
(`Depends(require_admin)` on the whole router). A device's owner is the
*student* carrying it (docs/V02_DESIGN.md §6: "only the device's owner (or
an admin)... The pager has no UI to add/edit/remove a number" -- the owner
manages the allow-list from the web app, same as an admin can).
"""

from __future__ import annotations

from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from pydantic import BaseModel, ConfigDict, field_validator

from app import devcfg
from app.auth import AuthedUser, require_user
from app.broker import BrokerClient
from app.store import devices as devices_store
from app.store import sms as sms_store
from app.store.devices import Device, DeviceStatus, SmsContact

router = APIRouter(prefix="/api/devices")


def get_broker(request: Request) -> BrokerClient:
    """Same `request.app.state.broker` dependency shape as
    `app/routers/admin.py`'s own `get_broker`."""
    return request.app.state.broker


def _is_admin(authed: AuthedUser) -> bool:
    return bool(authed.claims.get("admin")) or authed.user.role == "admin"


def _require_owner_or_admin(device_id: str, authed: AuthedUser) -> Device:
    """404 for an unknown device id (never distinguishing "doesn't exist"
    from "exists but isn't yours" for the *not found* case, same as
    `app/routers/conversations.py`'s `mark_read`), 403 for a signed-in,
    registered caller who is neither this device's owner nor an admin."""
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    if device.ownerUid != authed.uid and not _is_admin(authed):
        raise HTTPException(status_code=403, detail="not this device's owner")
    return device


# ---------------------------------------------------------------------------
# GET /api/devices -- the caller's own devices.
# ---------------------------------------------------------------------------


class DeviceSummary(BaseModel):
    id: str
    label: str
    status: DeviceStatus


@router.get("")
def list_my_devices(authed: Annotated[AuthedUser, Depends(require_user)]) -> list[DeviceSummary]:
    return [
        DeviceSummary(id=d.id, label=d.label, status=d.status)
        for d in devices_store.list_devices(owner_uid=authed.uid)
    ]


# ---------------------------------------------------------------------------
# SMS contacts -- docs/V02_DESIGN.md §6: `devices/{id}.smsContacts`, pushed
# as `/down cfg.sms` on every change (`app/devcfg.py`'s `push_sms_contacts`).
# ---------------------------------------------------------------------------


class SmsContactsRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    contacts: list[SmsContact]

    @field_validator("contacts")
    @classmethod
    def _check_contacts(cls, value: list[SmsContact]) -> list[SmsContact]:
        # Per-entry shape (name length/charset, E.164 phone) is already
        # enforced by `SmsContact`'s own field validators
        # (`app/store/devices.py`) -- this only checks the *list*-level
        # rules docs/V02_DESIGN.md §6 states: "Max 8 entries" and (§4's
        # scope note) unique phone numbers, so two entries can't silently
        # collide on which one the device actually dials/matches against.
        if len(value) > devices_store.MAX_SMS_CONTACTS:
            raise ValueError(
                f"at most {devices_store.MAX_SMS_CONTACTS} sms contacts are allowed, "
                f"got {len(value)}"
            )
        phones = [c.phone for c in value]
        if len(set(phones)) != len(phones):
            raise ValueError("sms contact phone numbers must be unique")
        return value


class SmsContactsResponse(BaseModel):
    contacts: list[SmsContact]
    # True iff a `cfg.sms` push is on its way to (or sitting unacked at) the
    # device -- i.e. what's stored here may not be what the pager has
    # actually applied yet.
    pending: bool


@router.get("/{device_id}/sms-contacts")
def get_sms_contacts(
    device_id: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> SmsContactsResponse:
    device = _require_owner_or_admin(device_id, authed)
    return SmsContactsResponse(
        contacts=device.smsContacts, pending=devcfg.sms_pending(device_id)
    )


@router.put("/{device_id}/sms-contacts")
def put_sms_contacts(
    device_id: str,
    req: SmsContactsRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> SmsContactsResponse:
    _require_owner_or_admin(device_id, authed)
    devices_store.set_sms_contacts(device_id, req.contacts)
    devcfg.push_sms_contacts(
        device_id, [c.model_dump() for c in req.contacts], broker
    )
    return SmsContactsResponse(contacts=req.contacts, pending=devcfg.sms_pending(device_id))


# ---------------------------------------------------------------------------
# SMS log -- docs/V02_DESIGN.md §6/§7: `devices/{id}/smsLog/{logId}`, audit
# only, never a thread entry (`app/store/sms.py`).
# ---------------------------------------------------------------------------


class SmsLogEntryOut(BaseModel):
    id: str
    ts: int
    smsTs: int
    dir: str
    peer: str
    name: str | None
    st: str
    body: str


class SmsLogResponse(BaseModel):
    entries: list[SmsLogEntryOut]


@router.get("/{device_id}/sms-log")
def get_sms_log(
    device_id: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
    before: int | None = None,
) -> SmsLogResponse:
    device = _require_owner_or_admin(device_id, authed)
    # `name` is resolved from the *current* contact list at read time (§6:
    # "name is resolved from the current contact list at read time") --
    # never stored on the log entry itself, so renaming/removing a contact
    # is reflected retroactively in old log rows rather than freezing
    # whatever name was current when the SMS happened.
    contacts_by_phone = {c.phone: c.name for c in device.smsContacts}
    entries = sms_store.list_log(device_id, limit=limit, before=before)
    return SmsLogResponse(
        entries=[
            SmsLogEntryOut(
                id=e.id,
                ts=e.ts,
                smsTs=e.smsTs,
                dir=e.dir,
                peer=e.peer,
                name=contacts_by_phone.get(e.peer),
                st=e.st,
                body=e.body,
            )
            for e in entries
        ]
    )

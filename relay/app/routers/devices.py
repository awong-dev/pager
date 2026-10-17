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
(`Depends(require_super)` on the whole router). A device's owner is the
*student* carrying it (docs/V02_DESIGN.md §6: "only the device's owner (or
an admin)... The pager has no UI to add/edit/remove a number" -- the owner
manages the allow-list from the web app, same as an admin can).
"""

from __future__ import annotations

import logging
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from pydantic import BaseModel, ConfigDict

from app import devcfg
from app.auth import AuthedUser, principal_for, require_user
from app.broker import BrokerClient
from app.store import devices as devices_store
from app.store import sms as sms_store
from app.store.devices import Device, DeviceStatus, SmsContact

logger = logging.getLogger("relay.devices")

router = APIRouter(prefix="/api/devices")


def get_broker(request: Request) -> BrokerClient:
    """Same `request.app.state.broker` dependency shape as
    `app/routers/admin.py`'s own `get_broker`."""
    return request.app.state.broker


def _is_family_admin_or_super(device: Device, authed: AuthedUser) -> bool:
    """docs/FAMILIES_DESIGN.md §4: "owner-or-admin ... becomes owner, or
    family admin of the device's `familyId`, or super." Claims-only, via
    `Principal` -- never `authed.user.role`/the legacy `admin` claim."""
    principal = principal_for(authed)
    if principal.role == "super":
        return True
    return principal.role == "admin" and principal.family_id == device.familyId


def _require_owner_or_admin(device_id: str, authed: AuthedUser) -> Device:
    """404 for an unknown device id (never distinguishing "doesn't exist"
    from "exists but isn't yours" for the *not found* case, same as
    `app/routers/conversations.py`'s `mark_read`), 403 for a signed-in,
    registered caller who is neither this device's owner, its family's
    admin, nor super."""
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    if device.ownerUid != authed.uid and not _is_family_admin_or_super(device, authed):
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
    device_id: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> None:
    """docs/FAMILIES_DESIGN.md §1 decision 11 / docs/FAMILIES_TASKS.md 3.2:
    `devices.smsContacts` is now a projection of the owning member's
    approved numbers (`PUT /api/family/members/{uid}/approved`,
    `app/routers/family.py`), not separately editable here -- this route
    stays mounted (rather than removed, and still behind `require_user`)
    only so an old client gets a clear, actionable error instead of a 404
    or a fully anonymous 405."""
    raise HTTPException(
        status_code=405, detail="Managed from People → Approved numbers"
    )


# ---------------------------------------------------------------------------
# GNSS enable -- docs/GNSS_DISABLE_DESIGN.md D5
# ---------------------------------------------------------------------------


class GnssConfig(BaseModel):
    model_config = ConfigDict(extra="forbid")

    en: bool


class GnssConfigResponse(BaseModel):
    en: bool
    pending: bool
    # Last `/status` `gnss` (0/1); None = never reported / older firmware.
    reported: int | None = None


def _gnss_response(device_id: str, device: Device) -> GnssConfigResponse:
    return GnssConfigResponse(
        en=devices_store.get_gnss_enabled(device_id),
        pending=devcfg.loc_pending(device_id),
        reported=device.status.gnss,
    )


@router.get("/{device_id}/gnss")
def get_gnss(
    device_id: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> GnssConfigResponse:
    device = _require_owner_or_admin(device_id, authed)
    return _gnss_response(device_id, device)


@router.put("/{device_id}/gnss")
def put_gnss(
    device_id: str,
    req: GnssConfig,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> GnssConfigResponse:
    device = _require_owner_or_admin(device_id, authed)
    devices_store.set_gnss_enabled(device_id, req.en)
    devcfg.push_loc(device_id, gnss=req.en, broker=broker)
    return _gnss_response(device_id, device)


# ---------------------------------------------------------------------------
# SMS log -- docs/V02_DESIGN.md §6/§7: `devices/{id}/smsLog/{logId}`, audit
# only, never a thread entry (`app/store/sms.py`).
# ---------------------------------------------------------------------------


class SmsLogEntryOut(BaseModel):
    id: str
    ts: int
    smsTs: int
    dir: str | None
    peer: str | None
    name: str | None
    st: str | None
    body: str | None
    # True for an audit-trail row of an sms_log upload that could not be
    # stored normally; `reason`/`rawHex` explain it. Old rows -> False.
    malformed: bool = False
    reason: str | None = None
    rawHex: str | None = None


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
                malformed=e.malformed,
                reason=e.reason,
                rawHex=e.rawHex,
            )
            for e in entries
        ]
    )

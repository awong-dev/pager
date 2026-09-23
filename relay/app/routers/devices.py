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

import logging
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from pydantic import BaseModel, ConfigDict, field_validator

from app import devcfg
from app.auth import AuthedUser, require_user
from app.broker import BrokerClient
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import sms as sms_store
from app.store.device_secrets import WifiNet
from app.store.devices import Device, DeviceStatus, SmsContact

logger = logging.getLogger("relay.devices")

router = APIRouter(prefix="/api/devices")

# docs/WIFI_DESIGN.md §4 exact sentence, echoed verbatim in the 409 body so
# the web app (docs/WIFI_TASKS.md W8) can show it directly rather than a
# generic error.
WIFI_TLS_NOT_PINNED_DETAIL = (
    "this device is not reporting a verified TLS connection; push a CA first"
)


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
# WiFi -- docs/WIFI_DESIGN.md §4/§6, docs/WIFI_TASKS.md W7:
# `deviceSecrets/{id}.wifiEnabled`/`.wifiNets` (never `devices/{id}` -- a
# PSK is a credential, and `devices/{id}` is readable by the device's own
# owner from the web app, `app/store/device_secrets.py`'s module docstring),
# pushed as `/down cfg.wifi` on every change (`app/devcfg.py`'s
# `push_wifi`).
# ---------------------------------------------------------------------------


class WifiConfigRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    en: bool
    # docs/WIFI_DESIGN.md §4: absent (`None`) means "leave the stored
    # networks alone, apply `en` only" -- the wire's own `cfg.wifi.nets`
    # semantics, carried through to this HTTP shape so the web app can
    # toggle WiFi without re-sending a PSK it was never given back by GET.
    # `[]` clears the stored networks; a non-empty list wholesale replaces
    # them. Per-entry shape (SSID/PSK byte bounds, no control characters) is
    # enforced by `WifiNet`'s own field validators.
    nets: list[WifiNet] | None = None

    @field_validator("nets")
    @classmethod
    def _check_nets(cls, value: list[WifiNet] | None) -> list[WifiNet] | None:
        if value is not None and len(value) > device_secrets_store.WIFI_MAX_NETS:
            raise ValueError(
                f"at most {device_secrets_store.WIFI_MAX_NETS} wifi networks are allowed, "
                f"got {len(value)}"
            )
        return value


class WifiNetOut(BaseModel):
    # docs/WIFI_TASKS.md W7: "GET never returns a stored PSK; it returns
    # {s, set: true}." `set` is always `true` for every entry this endpoint
    # ever returns -- `WifiNet` requires both `s` and `p` together (same
    # "both required" rule `wificred_parse_cfg_submap` enforces), so a stored
    # entry with a known SSID and no PSK can never exist -- but the field is
    # still modelled explicitly (not hardcoded into a bare list of strings)
    # so a future partial-entry state has somewhere to report `false`.
    s: str
    set: bool


class WifiConfigResponse(BaseModel):
    en: bool
    nets: list[WifiNetOut]
    # True iff a `cfg.wifi` push is on its way to (or sitting unacked at) the
    # device, same meaning as `SmsContactsResponse.pending` above.
    pending: bool


def _wifi_response(device_id: str) -> WifiConfigResponse:
    en, nets = device_secrets_store.get_wifi(device_id)
    return WifiConfigResponse(
        en=en,
        nets=[WifiNetOut(s=n.s, set=True) for n in nets],
        pending=devcfg.wifi_pending(device_id),
    )


@router.get("/{device_id}/wifi")
def get_wifi(
    device_id: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> WifiConfigResponse:
    _require_owner_or_admin(device_id, authed)
    return _wifi_response(device_id)


@router.put("/{device_id}/wifi")
def put_wifi(
    device_id: str,
    req: WifiConfigRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> WifiConfigResponse:
    device = _require_owner_or_admin(device_id, authed)
    if req.nets is not None and device.status.tls != "pinned":
        # docs/WIFI_DESIGN.md §4: pushing credentials to a device that is not
        # reporting a CA-pinned, validated TLS connection means sending a PSK
        # over a connection that cannot verify who is on the other end --
        # exactly the class of event `ingest.py`'s `tls == "broken"` edge
        # already logs as SECURITY (this is the push-side counterpart).
        logger.error(
            "SECURITY wifi-nets-refused device=%s tls=%s", device_id, device.status.tls
        )
        raise HTTPException(status_code=409, detail=WIFI_TLS_NOT_PINNED_DETAIL)
    device_secrets_store.set_wifi(device_id, en=req.en, nets=req.nets)
    devcfg.push_wifi(
        device_id,
        en=req.en,
        nets=[{"s": n.s, "p": n.p} for n in req.nets] if req.nets is not None else None,
        broker=broker,
    )
    return _wifi_response(device_id)


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

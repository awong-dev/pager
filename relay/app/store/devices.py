"""`devices/{deviceId}` -- docs/SERVER_PLAN.md §3, §5.5.

`locatableBy` is denormalised here (recomputed by `app/store/allow.py`
whenever the allow-list changes) so `firestore.rules` can check it without a
join. `status` is a single embedded map, overwritten wholesale on every
`/status` webhook (docs/PROTOCOL.md §5) -- there is no history, only the
latest.
"""

from __future__ import annotations

import re
import time
from datetime import datetime
from typing import Literal

from google.cloud.firestore import FieldFilter
from pydantic import BaseModel, ConfigDict, field_validator

from app.db.firestore import get_db
from app.wire import CellInfo

# docs/V02_DESIGN.md §6: `phone` is a real number, never an alias reference
# (unlike `contact_req`'s overloaded `ph`, §4.2) -- same E.164 shape
# duplicated across `app/wire.py`'s `_SMS_PEER_RE`, `app/ingest.py`'s
# `_PHONE_E164_RE` and `app/backends/sms_twilio.py`'s `_E164_RE`, for the
# same "not a public contract worth cross-module coupling" reason none of
# those import from each other either.
_SMS_PHONE_RE = re.compile(r"^\+[1-9]\d{6,14}$")
_CONTROL_CHAR_RE = re.compile(r"[\x00-\x1f\x7f]")
SMS_CONTACT_NAME_MAX_CODEPOINTS = 16
# docs/V02_DESIGN.md §6/§7: "Max 8 entries" is the design's own cap, but a
# `cfg.sms` push with 8 entries whose names use `book`/`contact_req`'s
# looser 48-UTF-8-byte cap does not fit in *signed JSON* (see
# `app/devcfg.py`'s `push_sms_contacts` docstring and this task's report for
# the byte arithmetic: 8 x 48-byte names is 762 signed-JSON bytes against the
# 640-byte envelope limit, PROTOCOL.md §3.3 -- `sig` really is only 11
# base64url characters at runtime (`app/devauth.py`'s 64-bit truncated tag),
# not the 44 an older, stale part of PROTOCOL.md §3.3's own worst-case table
# still shows for a pre-truncation, full-HMAC `sig`; flagged as a documentation
# discrepancy in this task's report, left alone as out of this task's scope).
# Rather than shrink the contact-count cap below the design's stated 8 (a
# more visible, more surprising change for a parent configuring the list),
# this tightens the *name* byte cap instead -- 24 UTF-8 bytes still fits
# every 16-codepoint ASCII name in full, and enough short non-Latin names
# (12 two/three-byte codepoints) for the common case, while guaranteeing 8
# maximal entries fit in both encodings with real margin (570/640 JSON,
# 421/640 CBOR -- see this task's report). Flagged there as a deliberate,
# documented deviation from the design text's unqualified "name <= 16 chars"
# rather than a silent one.
SMS_CONTACT_NAME_MAX_UTF8_BYTES = 24
MAX_SMS_CONTACTS = 8

# docs/DEVICE_PLAN.md §2.6 / docs/PROTOCOL.md §14.4: "more than 20 failures
# in 10 minutes on one device" raises `devices/{d}.status.authAlarm`.
AUTH_ALARM_THRESHOLD = 20
AUTH_ALARM_WINDOW_S = 600


class LastCell(BaseModel):
    """docs/PROTOCOL.md §13.2 (cell-tower location fallback, this task):
    the pager's most recently reported serving cell, recorded whenever a
    `/loc` envelope carries a `cell` field -- whether or not it resolved to
    a position (`app/cellgeo.py`) and whether or not the pager also had a
    real GNSS fix (`app/location.py`'s `ingest_loc`). Lets the web app show
    "last known cell" even with `CELL_GEO_PROVIDER=none` or a resolver
    failure/outage."""

    model_config = ConfigDict(extra="ignore")

    mcc: str
    mnc: str
    tac: int
    ci: int
    rsrp: int | None = None
    ts: int | None = None


class DeviceStatus(BaseModel):
    model_config = ConfigDict(extra="ignore")

    state: str | None = None
    mode: str | None = None
    battMv: int | None = None
    rssi: int | None = None
    session: str | None = None
    ts: int | None = None
    fw: str | None = None
    locPeriodS: int | None = None
    locMinS: int | None = None
    # docs/V02_DESIGN.md §4.3/§7 (CA trust): trust state and the pinned CA's
    # short fingerprint, per `V02_DESIGN.md` §4.3 ("Relay: persist both on
    # devices/{id}").
    tls: Literal["unpinned", "pinned", "broken"] | None = None
    caFp: str | None = None
    # docs/V02_DESIGN.md §5 (location): the device's own reported backoff.
    locBackoffS: int | None = None
    # docs/V02_DESIGN.md §6/§7 (device SMS): audit-queue drop counter.
    smsLost: int | None = None
    # docs/V02_DESIGN.md §9.5/§7 (this task): MQTT-session generation within
    # the current boot, stored next to `session` -- the online-edge
    # republish (ingest.py) treats a changed `link` exactly like a changed
    # `session`.
    link: int | None = None
    # docs/WIFI_DESIGN.md §6/§7, docs/PROTOCOL.md §5.1 (docs/WIFI_TASKS.md
    # W7): which physical transport carried this session -- display/
    # diagnosis only, same as `tls`/`ca_fp` above. Also read by
    # `app/routers/devices.py`'s `PUT /api/devices/{id}/wifi` guard, which
    # refuses to push `cfg.wifi.nets` unless `tls == "pinned"` (not `xport`
    # -- `xport` itself gates nothing, `tls` is the security-relevant field).
    xport: Literal["lte", "wifi"] | None = None
    # Crash diagnostics (docs/PROTOCOL.md §5.1, this task): display/
    # diagnosis only, same as `xport`/`tls` above -- the relay stores
    # whatever the device reports and never writes it back or acts on it.
    rst: int | None = None
    stage: int | None = None
    abn: int | None = None
    # Crash diagnostics (docs/PROTOCOL.md §5.1, this task): display/
    # diagnosis only, same as `rst`/`stage`/`abn` above -- the AT command
    # name the main loop was stuck on before the previous abnormal reset.
    stallcmd: str | None = None
    # docs/PROTOCOL.md §3.7/§5.1 (v0.4): "the relay stores it and sends
    # nudges only while the last online `/status` carried it" -- read by
    # `app/devcfg.py`'s `push_book` to gate the nudge. Only ever `1`
    # (`app/wire.py`'s `StatusEnvelope._check_bpull` drops any other value
    # back to `None` before it reaches here).
    bpull: int | None = None
    updatedAt: datetime | None = None
    # docs/DEVICE_PLAN.md §2.6: set once `sigFailures` crosses
    # AUTH_ALARM_THRESHOLD inside AUTH_ALARM_WINDOW_S; cleared on key
    # rotation.
    authAlarm: bool | None = None
    # docs/PROTOCOL.md §13.2 (this task): see `LastCell`'s docstring.
    lastCell: LastCell | None = None


class SmsContact(BaseModel):
    """docs/V02_DESIGN.md §6: one entry of `devices/{id}.smsContacts`, the
    parent-managed allow-list for the pager's own direct SMS path. `name`
    1-16 code points (no control characters); `phone` E.164. Uniqueness
    (`phone`) and the 8-entry cap are enforced by the API
    (`app/routers/devices.py`), not here -- this model only shapes one
    entry, the same "model the item, cap the list elsewhere" split
    `app/wire.py`'s `LocFix`/`LocEnvelope` use."""

    model_config = ConfigDict(extra="ignore")

    name: str
    phone: str

    @field_validator("name")
    @classmethod
    def _check_name(cls, value: str) -> str:
        if not (1 <= len(value) <= SMS_CONTACT_NAME_MAX_CODEPOINTS):
            raise ValueError(
                f"sms contact name {value!r} must be 1-{SMS_CONTACT_NAME_MAX_CODEPOINTS} "
                "characters"
            )
        if _CONTROL_CHAR_RE.search(value):
            raise ValueError(f"sms contact name {value!r} contains control characters")
        if len(value.encode("utf-8")) > SMS_CONTACT_NAME_MAX_UTF8_BYTES:
            raise ValueError(
                f"sms contact name {value!r} exceeds {SMS_CONTACT_NAME_MAX_UTF8_BYTES} UTF-8 bytes"
            )
        return value

    @field_validator("phone")
    @classmethod
    def _check_phone(cls, value: str) -> str:
        if not _SMS_PHONE_RE.match(value):
            raise ValueError(f"sms contact phone {value!r} is not a valid E.164 number")
        return value


class Device(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    ownerUid: str
    label: str
    mqttUsername: str
    defaultToUid: str | None = None
    revokedAt: datetime | None = None
    locatableBy: list[str] = []
    status: DeviceStatus = DeviceStatus()
    # docs/V02_DESIGN.md §6: the whole SMS contact allow-list, owner/admin
    # managed only (the pager itself has no UI to add/edit/remove a number).
    # Pushed to the device as `/down cfg.sms` on every change
    # (`app/devcfg.py`'s `push_sms_contacts`) -- this field is the relay's
    # own source of truth, independent of whatever the device has actually
    # applied/acked (`devices/{d}.pendingCfgSms` tracks that, same shape as
    # `pendingBook`/`pendingCfg`/`pendingCfgCa`).
    smsContacts: list[SmsContact] = []
    # Carrier APN baked into this device's setup codes and bundles
    # (app/apn_presets.py). None = carrier default.
    apn: str | None = None
    # docs/DEVICE_PLAN.md §2.6/§14: "password" is the v1, unsigned device;
    # "hmac" is a device provisioned with a `deviceSecrets/{d}` key that
    # signs every `/up`, `/status` and `/loc` envelope (app/devauth.py).
    # Defaults to "hmac" for devices created from here on -- see
    # `create_device`'s own `auth_mode` parameter for how existing
    # password-mode test/dev devices opt out.
    authMode: Literal["password", "hmac"] = "hmac"
    # docs/DEVICE_PLAN.md §2.4: the encoding of the device's last *accepted*
    # inbound envelope -- the relay answers on `/down` in the same encoding.
    # `None` until the device's first accepted envelope.
    wire: Literal["json", "cbor"] | None = None
    # docs/DEVICE_PLAN.md §3.2 step 1 / §3.5 (docs/DEVICE_TASKS.md S2.2):
    # "issued" is written the moment a setup code exists for this device
    # (`POST /api/admin/devices`, and again on
    # `POST /api/admin/devices/{id}/rotate-credentials`, since a rotated
    # device needs a fresh bootstrap fetch too); a later task
    # (docs/DEVICE_TASKS.md S2b.3, `pager/boot/+/up` handling in
    # app/ingest.py) flips it to "provisioned" once the first *signed*
    # `/status` from that device arrives. `None` only for devices created
    # before this field existed.
    provisionState: Literal["issued", "provisioned"] | None = None


def _devices():
    return get_db().collection("devices")


def create_device(
    *,
    device_id: str,
    owner_uid: str,
    label: str,
    mqtt_username: str,
    mqtt_password_hash: str,
    default_to_uid: str | None = None,
    auth_mode: Literal["password", "hmac"] = "hmac",
    apn: str | None = None,
) -> Device:
    """`mqtt_password_hash` is accepted but no longer written anywhere
    (docs/DEVICE_TASKS.md S2.2, per S1.1's own note that `devices/{d}`
    should stop storing it): the only place an MQTT credential's hash
    belongs is `deviceSecrets/{d}` (`app/store/device_secrets.py`'s
    `create`/`rotate`), which nothing but firebase-admin can ever read
    (`docs/DEVICE_PLAN.md` §2.6). The parameter itself stays -- and stays
    required -- purely so the many call sites across this test suite
    outside S2.2's `Files` list (`tests/test_ingest.py`,
    `tests/test_webhooks.py`, `tests/test_rules.py`, etc., all of which pass
    a throwaway `mqtt_password_hash="x"` to build a fixture device) do not
    need editing for a value they never asserted on in the first place;
    `POST /api/admin/devices` (`app/routers/admin.py`) is this function's
    only real caller and passes the true hash to `device_secrets_store.
    create` instead."""
    ref = _devices().document(device_id)
    ref.set(
        {
            "ownerUid": owner_uid,
            "label": label,
            "apn": apn,
            "mqttUsername": mqtt_username,
            "defaultToUid": default_to_uid,
            "revokedAt": None,
            "locatableBy": [],
            "status": {},
            "authMode": auth_mode,
            "wire": None,
            "provisionState": "issued",
        }
    )
    fetched = get_device(device_id)
    assert fetched is not None
    return fetched


def get_device(device_id: str) -> Device | None:
    snap = _devices().document(device_id).get()
    if not snap.exists:
        return None
    return Device.model_validate({"id": device_id, **(snap.to_dict() or {})})


def list_devices(owner_uid: str | None = None) -> list[Device]:
    query = _devices()
    if owner_uid is not None:
        query = query.where(filter=FieldFilter("ownerUid", "==", owner_uid))
    return [
        Device.model_validate({"id": snap.id, **(snap.to_dict() or {})}) for snap in query.stream()
    ]


def delete_device(device_id: str) -> None:
    _devices().document(device_id).delete()


def revoke_device(device_id: str) -> Device:
    from google.cloud.firestore import SERVER_TIMESTAMP

    _devices().document(device_id).update({"revokedAt": SERVER_TIMESTAMP})
    fetched = get_device(device_id)
    if fetched is None:
        raise KeyError(f"no such device: {device_id!r}")
    return fetched


def set_locatable_by(device_id: str, uids: list[str]) -> None:
    _devices().document(device_id).update({"locatableBy": uids})


def set_sms_contacts(device_id: str, contacts: list[SmsContact]) -> None:
    """docs/V02_DESIGN.md §6: `PUT /api/devices/{id}/sms-contacts`'s writer
    (`app/routers/devices.py`) -- the API has already validated max-8/
    unique-phone/shape before calling this, so this function just stores
    whatever list it is given, the whole list replacing the old one (never
    merged/appended -- an SMS contact removed in the request must actually
    disappear)."""
    _devices().document(device_id).set(
        {"smsContacts": [c.model_dump() for c in contacts]}, merge=True
    )


def set_apn(device_id: str, apn: str | None) -> None:
    """The caller has validated `apn` (app/apn_presets.validate_apn). Takes
    effect at the device's next setup code; nothing is pushed to the pager."""
    _devices().document(device_id).set({"apn": apn}, merge=True)


def set_provision_state(device_id: str, state: Literal["issued", "provisioned"]) -> None:
    """docs/DEVICE_PLAN.md §3.2/§3.5: written `"issued"` by
    `POST /api/admin/devices` and again by
    `POST /api/admin/devices/{id}/rotate-credentials` (a rotated device
    needs a fresh bootstrap fetch, same as a brand-new one); flipped to
    `"provisioned"` on the device's first signed `/status` by a later task
    (docs/DEVICE_TASKS.md S2b.3)."""
    _devices().document(device_id).set({"provisionState": state}, merge=True)


def set_wire(device_id: str, wire: Literal["json", "cbor"]) -> None:
    """docs/DEVICE_PLAN.md §2.4: set from each *accepted* inbound envelope's
    encoding (app/ingest.py), so the relay's next `/down` publish answers in
    the same encoding as the device's last message."""
    _devices().document(device_id).set({"wire": wire}, merge=True)


def record_sig_failure(device_id: str, now: float | None = None) -> int:
    """docs/DEVICE_PLAN.md §2.6 / docs/PROTOCOL.md §14.4's "more than 20
    failures in 10 minutes" alert needs a time-windowed count.
    `deviceSecrets/{d}.sigFailures` (app/store/device_secrets.py) is a
    lifetime counter with no time window, and that module is outside this
    task's `Files` list -- so the window is tracked here instead, as a
    small capped list of failure timestamps on `devices/{d}` itself (server
    bookkeeping, not part of the wire protocol). Deliberately not a
    transaction: this is a best-effort alert (the security boundary is the
    signature check that produced the failure, not this counter), so an
    occasional missed/duplicate count under concurrent failures from the
    same device is an acceptable trade for not adding a second transaction
    to the hot ingest path.

    Returns how many failures fall inside the trailing
    `AUTH_ALARM_WINDOW_S`-second window, including this one."""
    now = now if now is not None else time.time()
    ref = _devices().document(device_id)
    snap = ref.get()
    data = (snap.to_dict() or {}) if snap.exists else {}
    cutoff = now - AUTH_ALARM_WINDOW_S
    times = [
        t for t in data.get("authFailureTimes", []) if isinstance(t, (int, float)) and t > cutoff
    ]
    times.append(now)
    # Capped so a sustained attack cannot grow this list without bound; only
    # the most recent AUTH_ALARM_THRESHOLD failures are ever relevant to the
    # threshold check anyway.
    times = times[-AUTH_ALARM_THRESHOLD:]
    ref.set({"authFailureTimes": times}, merge=True)
    return len(times)


def set_auth_alarm(device_id: str, value: bool) -> None:
    _devices().document(device_id).set({"status": {"authAlarm": value}}, merge=True)


def clear_auth_alarm(device_id: str) -> None:
    """docs/DEVICE_PLAN.md §2.6: cleared on key rotation. Not called from
    anywhere yet -- there is no rotation endpoint in this task's scope.
    TODO(orchestrator): wire this (and resetting `authFailureTimes`) into
    the key-rotation task once it exists."""
    _devices().document(device_id).set(
        {"authFailureTimes": [], "status": {"authAlarm": False}}, merge=True
    )


def set_last_cell(device_id: str, cell: CellInfo, ts: int) -> None:
    """docs/PROTOCOL.md §13.2 (this task): called by `app/location.py`'s
    `ingest_loc` whenever a `/loc` envelope carries a `cell` field, whether
    or not `app/cellgeo.py` resolves it to a position and whether or not the
    pager also sent a real GNSS fix -- "the cell is only recorded" in that
    second case. A merge write (like `set_auth_alarm`/`update_status`
    above), so this never disturbs the rest of `status`."""
    _devices().document(device_id).set(
        {
            "status": {
                "lastCell": {
                    "mcc": cell.mcc,
                    "mnc": cell.mnc,
                    "tac": cell.tac,
                    "ci": cell.ci,
                    "rsrp": cell.rsrp,
                    "ts": ts,
                }
            }
        },
        merge=True,
    )


def update_status(device_id: str, **fields: object) -> None:
    """Overwrites `status` wholesale with `fields` + `updatedAt`. Caller
    (ingest.py's Firestore-backed successor) passes exactly the fields it
    has; unset ones are omitted, not defaulted, so a partial `/status`
    payload cannot stomp a previously-known field with a bogus `None` --
    callers that want that must pass it explicitly."""
    from google.cloud.firestore import SERVER_TIMESTAMP

    status = {k: v for k, v in fields.items() if v is not None}
    status["updatedAt"] = SERVER_TIMESTAMP
    _devices().document(device_id).set({"status": status}, merge=True)

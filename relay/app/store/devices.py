"""`devices/{deviceId}` -- docs/SERVER_PLAN.md §3, §5.5.

`locatableBy` is denormalised here (recomputed by `app/store/allow.py`
whenever the allow-list changes) so `firestore.rules` can check it without a
join. `status` is a single embedded map, overwritten wholesale on every
`/status` webhook (docs/PROTOCOL.md §5) -- there is no history, only the
latest.
"""

from __future__ import annotations

import time
from datetime import datetime
from typing import Literal

from google.cloud.firestore import FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

# docs/DEVICE_PLAN.md §2.6 / docs/PROTOCOL.md §14.4: "more than 20 failures
# in 10 minutes on one device" raises `devices/{d}.status.authAlarm`.
AUTH_ALARM_THRESHOLD = 20
AUTH_ALARM_WINDOW_S = 600


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
    # short fingerprint, per `CA_TRUST_PLAN.md` §3.3 ("Relay: persist both on
    # devices/{id}").
    tls: Literal["unpinned", "pinned", "broken"] | None = None
    caFp: str | None = None
    # docs/V02_DESIGN.md §5 (location): the device's own reported backoff.
    locBackoffS: int | None = None
    # docs/V02_DESIGN.md §6/§7 (device SMS): audit-queue drop counter.
    smsLost: int | None = None
    updatedAt: datetime | None = None
    # docs/DEVICE_PLAN.md §2.6: set once `sigFailures` crosses
    # AUTH_ALARM_THRESHOLD inside AUTH_ALARM_WINDOW_S; cleared on key
    # rotation.
    authAlarm: bool | None = None


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

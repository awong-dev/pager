"""`devices/{deviceId}` -- docs/SERVER_PLAN.md §3, §5.5.

`locatableBy` is denormalised here (recomputed by `app/store/allow.py`
whenever the allow-list changes) so `firestore.rules` can check it without a
join. `status` is a single embedded map, overwritten wholesale on every
`/status` webhook (docs/PROTOCOL.md §5) -- there is no history, only the
latest.
"""

from __future__ import annotations

from datetime import datetime

from google.cloud.firestore import FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db


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
    updatedAt: datetime | None = None


class Device(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    ownerUid: str
    label: str
    mqttUsername: str
    mqttPasswordHash: str
    defaultToUid: str | None = None
    revokedAt: datetime | None = None
    locatableBy: list[str] = []
    status: DeviceStatus = DeviceStatus()


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
) -> Device:
    ref = _devices().document(device_id)
    ref.set(
        {
            "ownerUid": owner_uid,
            "label": label,
            "mqttUsername": mqtt_username,
            "mqttPasswordHash": mqtt_password_hash,
            "defaultToUid": default_to_uid,
            "revokedAt": None,
            "locatableBy": [],
            "status": {},
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


def set_mqtt_password_hash(device_id: str, password_hash: str) -> None:
    _devices().document(device_id).update({"mqttPasswordHash": password_hash})


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

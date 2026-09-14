"""`/api/admin/*` -- docs/SERVER_PLAN.md §5.1, §5.5. Every route here is
gated on `app.auth.require_admin` (the `admin` custom claim, refreshed by
`app.auth`/`app.bootstrap`).

Admin creates users *ahead of time* by email/phone (docs/SERVER_PLAN.md
§5.3's registry gate): `POST /api/admin/users` creates both the Firebase
Auth account and the `users/{uid}` Firestore document in one call, so there
is never a window where an Auth account exists without a matching registry
row. Device creation returns the generated MQTT password exactly once and
stores only its hash (`hashlib.sha256` -- no new dependency, per
docs/SERVER_PLAN.md §5.5).
"""

from __future__ import annotations

import hashlib
import secrets
from typing import Literal

from fastapi import APIRouter, Depends, HTTPException
from firebase_admin import auth as fb_auth
from pydantic import BaseModel, ConfigDict

from app.auth import require_admin
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import settings as settings_store
from app.store import users as users_store
from app.store.allow import AllowEdge, EdgeInput
from app.store.devices import Device
from app.store.settings import RetentionSetting, RetentionSettings
from app.store.users import User

router = APIRouter(prefix="/api/admin", dependencies=[Depends(require_admin)])

MQTT_PASSWORD_BYTES = 24


# ---------------------------------------------------------------------------
# users
# ---------------------------------------------------------------------------


class CreateUserRequest(BaseModel):
    alias: str
    displayName: str
    email: str | None = None
    phone: str | None = None
    role: Literal["admin", "member"] = "member"
    uid: str | None = None  # optional: pin a specific uid (tests, re-runs)


class PatchUserRequest(BaseModel):
    displayName: str | None = None
    email: str | None = None
    phone: str | None = None
    role: Literal["admin", "member"] | None = None
    disabled: bool | None = None


def _set_admin_claim(uid: str, is_admin: bool) -> None:
    fb_auth.set_custom_user_claims(uid, {"admin": True} if is_admin else {})


@router.post("/users")
def create_user(req: CreateUserRequest) -> User:
    if not req.email and not req.phone:
        raise HTTPException(status_code=400, detail="email or phone is required")

    kwargs: dict[str, object] = {}
    if req.uid:
        kwargs["uid"] = req.uid
    if req.email:
        kwargs["email"] = req.email
    if req.phone:
        kwargs["phone_number"] = req.phone
    try:
        auth_user = fb_auth.create_user(**kwargs)
    except fb_auth.EmailAlreadyExistsError as exc:
        raise HTTPException(status_code=409, detail="email already registered") from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    try:
        user = users_store.create_user(
            uid=auth_user.uid,
            alias=req.alias,
            display_name=req.displayName,
            email=req.email,
            phone=req.phone,
            role=req.role,
        )
    except (users_store.AliasTaken, users_store.InvalidAlias) as exc:
        # Roll back the just-created Auth account so a rejected alias
        # doesn't leave an orphaned, unregistered Auth user behind (which
        # the registry gate would otherwise 403 forever with no UI to fix).
        fb_auth.delete_user(auth_user.uid)
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    if req.role == "admin":
        _set_admin_claim(user.uid, True)
    return user


@router.get("/users")
def list_users() -> list[User]:
    return users_store.list_users()


@router.patch("/users/{uid}")
def patch_user(uid: str, req: PatchUserRequest) -> User:
    if users_store.get_user(uid) is None:
        raise HTTPException(status_code=404, detail="no such user")
    user = users_store.update_user(
        uid,
        display_name=req.displayName,
        email=req.email,
        phone=req.phone,
        role=req.role,
        disabled=req.disabled,
    )
    if req.role is not None:
        _set_admin_claim(uid, req.role == "admin")
    return user


@router.delete("/users/{uid}")
def delete_user(uid: str) -> dict[str, bool]:
    if users_store.get_user(uid) is None:
        raise HTTPException(status_code=404, detail="no such user")
    users_store.delete_user(uid)
    try:
        fb_auth.delete_user(uid)
    except fb_auth.UserNotFoundError:
        pass
    return {"ok": True}


# ---------------------------------------------------------------------------
# allowlist
# ---------------------------------------------------------------------------


class AllowlistEntry(BaseModel):
    model_config = ConfigDict(extra="ignore")

    fromAlias: str
    toAlias: str
    message: bool = True
    locate: bool = True


class PutAllowlistRequest(BaseModel):
    entries: list[AllowlistEntry]


def _resolve_uid(alias: str) -> str:
    uid = users_store.get_uid_for_alias(alias)
    if uid is None:
        raise HTTPException(status_code=400, detail=f"unknown alias: {alias!r}")
    return uid


@router.put("/allowlist")
def put_allowlist(req: PutAllowlistRequest) -> list[AllowEdge]:
    edges = [
        EdgeInput(
            from_uid=_resolve_uid(e.fromAlias),
            to_uid=_resolve_uid(e.toAlias),
            message=e.message,
            locate=e.locate,
        )
        for e in req.entries
    ]
    return allow_store.replace_all(edges)


@router.get("/allowlist")
def get_allowlist() -> list[AllowEdge]:
    return allow_store.list_edges()


# ---------------------------------------------------------------------------
# devices
# ---------------------------------------------------------------------------


class CreateDeviceRequest(BaseModel):
    deviceId: str
    ownerAlias: str
    label: str
    defaultToAlias: str | None = None


class CreateDeviceResponse(BaseModel):
    device: Device
    mqttUsername: str
    mqttPassword: str  # returned exactly once, per docs/SERVER_PLAN.md §5.5


@router.post("/devices")
def create_device(req: CreateDeviceRequest) -> CreateDeviceResponse:
    owner_uid = _resolve_uid(req.ownerAlias)
    default_to_uid = _resolve_uid(req.defaultToAlias) if req.defaultToAlias else None
    if devices_store.get_device(req.deviceId) is not None:
        raise HTTPException(status_code=409, detail="device already exists")

    password = secrets.token_urlsafe(MQTT_PASSWORD_BYTES)
    password_hash = hashlib.sha256(password.encode("utf-8")).hexdigest()
    device = devices_store.create_device(
        device_id=req.deviceId,
        owner_uid=owner_uid,
        label=req.label,
        mqtt_username=req.deviceId,
        mqtt_password_hash=password_hash,
        default_to_uid=default_to_uid,
    )
    # docs/SERVER_PLAN.md §2 decision 4: "the pager device is modelled as
    # just another delivery backend of its owner" -- `app/routing.py`'s
    # fan-out finds a device to publish to by looking at the owner's
    # `kind='pager'` backends, not the `devices` collection directly, so
    # creating the device without this would leave it undeliverable.
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": req.deviceId}, enabled=True
    )
    # `(build finding, Phase 4)`: a fresh device always starts at
    # `locatableBy: []` (`devices_store.create_device`); if the owner
    # already has incoming `locate` edges from an allow-list set up
    # *before* this device existed, this device would otherwise never pick
    # them up (`allow_store.set_edge`/`replace_all` only recompute
    # `locatableBy` on devices that exist when an edge changes). See
    # `allow_store.recompute_locatable_by_for_owner`'s docstring.
    allow_store.recompute_locatable_by_for_owner(owner_uid)
    device = devices_store.get_device(req.deviceId)
    assert device is not None
    return CreateDeviceResponse(device=device, mqttUsername=req.deviceId, mqttPassword=password)


@router.get("/devices")
def list_devices() -> list[Device]:
    return devices_store.list_devices()


@router.delete("/devices/{device_id}")
def delete_device(device_id: str) -> dict[str, bool]:
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    for b in backends_store.list_backends(device.ownerUid):
        if b.kind == "pager" and b.config.get("deviceId") == device_id:
            backends_store.delete_backend(device.ownerUid, b.id)
    devices_store.delete_device(device_id)
    return {"ok": True}


class RotateCredentialsResponse(BaseModel):
    mqttUsername: str
    mqttPassword: str


@router.post("/devices/{device_id}/rotate-credentials")
def rotate_credentials(device_id: str) -> RotateCredentialsResponse:
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    password = secrets.token_urlsafe(MQTT_PASSWORD_BYTES)
    password_hash = hashlib.sha256(password.encode("utf-8")).hexdigest()
    devices_store.set_mqtt_password_hash(device_id, password_hash)
    return RotateCredentialsResponse(mqttUsername=device.mqttUsername, mqttPassword=password)


# ---------------------------------------------------------------------------
# settings
# ---------------------------------------------------------------------------


class PutSettingsRequest(BaseModel):
    messages: RetentionSetting
    locations: RetentionSetting


@router.put("/settings")
def put_settings(req: PutSettingsRequest) -> RetentionSettings:
    return settings_store.set_retention(messages=req.messages, locations=req.locations)


@router.get("/settings")
def get_settings() -> RetentionSettings:
    return settings_store.get_retention()

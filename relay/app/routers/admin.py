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

Every **write** route here (not the
`GET`s -- reads are already the cheapest, least dangerous thing this router
does) also carries `Depends(require_admin_write_rate_limit)`, a coarser,
defense-in-depth sibling of `POST /api/me/backends`'s rate limit
(`app/routers/me.py`): this surface is already admin-claim-gated, so the risk
here is a compromised/scripted admin session or a buggy admin-UI retry loop
hammering Firestore, not an anonymous attacker. `app/store/rate_limits.py`'s
same fixed-window counter, keyed `"admin:{uid}"` (shared across every write
route below, on purpose -- one admin's overall write rate is what's bounded,
not each route independently), `RATE_LIMIT_ADMIN_WRITE_LIMIT` calls per
`RATE_LIMIT_ADMIN_WRITE_WINDOW_S` seconds (defaults: 60 per minute).
"""

from __future__ import annotations

import hashlib
import os
import secrets
from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException
from firebase_admin import auth as fb_auth
from pydantic import BaseModel, ConfigDict

from app.auth import AuthedUser, require_admin
from app.backends.sms_twilio import normalize_e164
from app.db.firestore import get_db
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import devices as devices_store
from app.store import rate_limits as rate_limits_store
from app.store import settings as settings_store
from app.store import users as users_store
from app.store.allow import AllowEdge, EdgeInput
from app.store.backends import Backend
from app.store.contacts import ContactRequest
from app.store.devices import Device
from app.store.settings import RetentionSetting, RetentionSettings
from app.store.users import ALIAS_RE, User

router = APIRouter(prefix="/api/admin", dependencies=[Depends(require_admin)])

MQTT_PASSWORD_BYTES = 24

DEFAULT_ADMIN_WRITE_LIMIT = 60
DEFAULT_ADMIN_WRITE_WINDOW_S = 60


def _admin_write_rate_limit() -> tuple[int, int]:
    limit = int(os.environ.get("RATE_LIMIT_ADMIN_WRITE_LIMIT", str(DEFAULT_ADMIN_WRITE_LIMIT)))
    window_s = int(
        os.environ.get("RATE_LIMIT_ADMIN_WRITE_WINDOW_S", str(DEFAULT_ADMIN_WRITE_WINDOW_S))
    )
    return limit, window_s


def require_admin_write_rate_limit(
    authed: Annotated[AuthedUser, Depends(require_admin)],
) -> None:
    """`Depends(require_admin)` here is the *same* callable the router-level
    dependency already ran for this request, so FastAPI's per-request
    dependency cache (the default `use_cache=True`) returns the cached
    `AuthedUser` rather than re-verifying the bearer token a second time --
    this dependency only adds the rate-limit check on top."""
    limit, window_s = _admin_write_rate_limit()
    if not rate_limits_store.check_and_increment(
        f"admin:{authed.uid}", limit=limit, window_s=window_s
    ):
        raise HTTPException(status_code=429, detail="admin rate limit exceeded; try again later")


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


@router.post("/users", dependencies=[Depends(require_admin_write_rate_limit)])
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


@router.patch("/users/{uid}", dependencies=[Depends(require_admin_write_rate_limit)])
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


@router.delete("/users/{uid}", dependencies=[Depends(require_admin_write_rate_limit)])
def delete_user(uid: str) -> dict[str, bool]:
    if users_store.get_user(uid) is None:
        raise HTTPException(status_code=404, detail="no such user")
    users_store.delete_user(uid)
    try:
        fb_auth.delete_user(uid)
    except fb_auth.UserNotFoundError:
        pass
    return {"ok": True}


class CreateBackendRequest(BaseModel):
    kind: Literal["sms"]
    phone: str


def _create_admin_asserted_backend(uid: str, phone: str) -> Backend:
    """Shared by `POST /api/admin/users/{uid}/backends` and the `create`
    approval path below (docs/DEVICE_PLAN.md §4.3): an `sms` backend
    asserted verified by an admin rather than by the usual code-verification
    flow (`app/routers/me.py`'s `POST /api/me/backends/{id}/verify`) --
    `verifiedAt` set and `phoneIndex` written immediately, same as a real
    verification, plus `adminVerified: true` recording *how* it got that
    way. `Backend` (`app/store/backends.py`) does not model `adminVerified`
    -- that module is outside this task's (S4.1) `Files` list -- so it is
    written directly on the Firestore doc here (dropped on read by
    `Backend.model_validate`'s `extra='ignore'` until backends.py adds the
    field)."""
    normalized = normalize_e164(phone)
    backend = backends_store.create_backend(uid, kind="sms", config={"phone": normalized})
    backends_store.update_backend(uid, backend.id, verified=True)
    backends_store.set_phone_index(normalized, uid, backend.id)
    get_db().collection("users").document(uid).collection("backends").document(backend.id).update(
        {"adminVerified": True}
    )
    fetched = backends_store.get_backend(uid, backend.id)
    assert fetched is not None
    return fetched


@router.post(
    "/users/{uid}/backends", dependencies=[Depends(require_admin_write_rate_limit)]
)
def create_user_backend(uid: str, req: CreateBackendRequest) -> Backend:
    if users_store.get_user(uid) is None:
        raise HTTPException(status_code=404, detail="no such user")
    try:
        return _create_admin_asserted_backend(uid, req.phone)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


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


@router.put("/allowlist", dependencies=[Depends(require_admin_write_rate_limit)])
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


@router.post("/devices", dependencies=[Depends(require_admin_write_rate_limit)])
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
    # A fresh device always starts at
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


@router.delete("/devices/{device_id}", dependencies=[Depends(require_admin_write_rate_limit)])
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


@router.post("/devices/{device_id}/rotate-credentials", dependencies=[Depends(require_admin_write_rate_limit)])
def rotate_credentials(device_id: str) -> RotateCredentialsResponse:
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    password = secrets.token_urlsafe(MQTT_PASSWORD_BYTES)
    password_hash = hashlib.sha256(password.encode("utf-8")).hexdigest()
    devices_store.set_mqtt_password_hash(device_id, password_hash)
    return RotateCredentialsResponse(mqttUsername=device.mqttUsername, mqttPassword=password)


# ---------------------------------------------------------------------------
# contacts -- docs/DEVICE_PLAN.md §4.1-4.3, docs/DEVICE_TASKS.md S4.1.
#
# A `contactRequests/{deviceId}_{reqId}` row is created by `app.ingest.Ingest`
# from the device's own `/up kind:"contact_req"` (§4.2); approval/rejection
# is admin-only (§4.3: "the device owner is the student, who must not be able
# to approve their own recipients"). Every decision below bumps
# `devices/{d}.bookVersion` and calls `contacts_store.push_book` -- the
# latter is a placeholder until S4.2 (see `app/store/contacts.py`).
# ---------------------------------------------------------------------------


class ContactApproveRequest(BaseModel):
    mode: Literal["link", "create"]
    alias: str | None = None
    locate: bool = False


class ContactRejectRequest(BaseModel):
    reason: str


def _slugify_name(name: str) -> str | None:
    """docs/DEVICE_PLAN.md §4.3: "alias defaults to a slug of `name` when one
    can be derived (a CJK name yields none, so the admin types the alias)."
    A conservative subset of `ALIAS_RE` (`app/store/users.py`) -- lowercase
    ASCII letters/digits only, truncated to 16 -- so the result never needs
    a leading-character special case; `None` when nothing survives (e.g. an
    all-CJK name), which the caller treats as "the admin must supply one"."""
    slug = "".join(ch for ch in name.lower() if ch.isascii() and ch.isalnum())[:16]
    return slug or None


def _resolve_link_uid(request: ContactRequest, admin_alias: str | None) -> str | None:
    """docs/DEVICE_PLAN.md §4.3's "link to existing user": a verified `sms`
    backend's owner (via `phoneIndex`) takes priority over an alias match,
    tried first against the request's own `alias` reference (§4.2's
    "alias the student already knows") and then against the alias the admin
    typed into the approval dialog."""
    if request.phone is not None:
        found = backends_store.get_by_phone(request.phone)
        if found is not None:
            return found[0]
    for alias in (request.alias, admin_alias):
        if alias:
            uid = users_store.get_uid_for_alias(alias)
            if uid is not None:
                return uid
    return None


@router.get("/contacts")
def list_contacts(
    status: Literal["pending", "approved", "rejected"] | None = None,
) -> list[ContactRequest]:
    return contacts_store.list_requests(status=status)


@router.post("/contacts/{key}/approve", dependencies=[Depends(require_admin_write_rate_limit)])
def approve_contact(
    key: str,
    req: ContactApproveRequest,
    authed: Annotated[AuthedUser, Depends(require_admin)],
) -> ContactRequest:
    request = contacts_store.get_request(key)
    if request is None:
        raise HTTPException(status_code=404, detail="no such contact request")
    if request.status != "pending":
        raise HTTPException(status_code=409, detail="contact request already decided")

    if req.mode == "link":
        contact_uid = _resolve_link_uid(request, req.alias)
        if contact_uid is None:
            raise HTTPException(status_code=400, detail="no existing user found to link to")
    else:
        alias = req.alias or _slugify_name(request.name)
        if not alias or not ALIAS_RE.match(alias):
            raise HTTPException(
                status_code=400,
                detail="an alias is required to create a new user for this contact",
            )
        auth_user = fb_auth.create_user()
        try:
            new_user = users_store.create_user(
                uid=auth_user.uid, alias=alias, display_name=request.name
            )
        except (users_store.AliasTaken, users_store.InvalidAlias) as exc:
            # Same rollback `create_user` (the `/users` route above) already
            # does: don't leave an orphaned, unregistered Auth account.
            fb_auth.delete_user(auth_user.uid)
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        contact_uid = new_user.uid
        if request.phone is not None:
            _create_admin_asserted_backend(contact_uid, request.phone)

    # §4.3: "upsert two allow edges (owner -> contact `message`, contact ->
    # owner `message`; `locate` is a separate checkbox, default off)".
    owner_uid = request.ownerUid
    allow_store.set_edge(owner_uid, contact_uid, message=True, locate=req.locate)
    allow_store.set_edge(contact_uid, owner_uid, message=True, locate=req.locate)
    # `set_edge` already recomputes `locatableBy` for each edge's `to_uid`
    # (i.e. both directions here); called again explicitly per this task's
    # `Do` steps, matching `POST /api/admin/devices`'s own belt-and-suspenders
    # call after `set_edge`/`replace_all` (`allow_store.
    # recompute_locatable_by_for_owner`'s docstring).
    allow_store.recompute_locatable_by_for_owner(owner_uid)
    allow_store.recompute_locatable_by_for_owner(contact_uid)

    updated = contacts_store.approve(key, decided_by=authed.uid)
    contacts_store.bump_book_version(request.deviceId)
    contacts_store.push_book(request.deviceId)
    return updated


@router.post("/contacts/{key}/reject", dependencies=[Depends(require_admin_write_rate_limit)])
def reject_contact(
    key: str,
    req: ContactRejectRequest,
    authed: Annotated[AuthedUser, Depends(require_admin)],
) -> ContactRequest:
    request = contacts_store.get_request(key)
    if request is None:
        raise HTTPException(status_code=404, detail="no such contact request")
    if request.status != "pending":
        raise HTTPException(status_code=409, detail="contact request already decided")

    updated = contacts_store.reject(key, reason=req.reason, decided_by=authed.uid)
    contacts_store.bump_book_version(request.deviceId)
    contacts_store.push_book(request.deviceId)
    return updated


# ---------------------------------------------------------------------------
# settings
# ---------------------------------------------------------------------------


class PutSettingsRequest(BaseModel):
    messages: RetentionSetting
    locations: RetentionSetting


@router.put("/settings", dependencies=[Depends(require_admin_write_rate_limit)])
def put_settings(req: PutSettingsRequest) -> RetentionSettings:
    return settings_store.set_retention(messages=req.messages, locations=req.locations)


@router.get("/settings")
def get_settings() -> RetentionSettings:
    return settings_store.get_retention()

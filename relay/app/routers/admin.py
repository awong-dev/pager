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
from datetime import UTC, datetime, timedelta
from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException, Request
from firebase_admin import auth as fb_auth
from pydantic import BaseModel, ConfigDict, Field

from app import apn_presets, ca_resolve, devcfg, devsetup
from app.auth import AuthedUser, require_admin
from app.backends.sms_twilio import normalize_e164
from app.broker import BrokerClient
from app.config import Settings
from app.db.firestore import get_db
from app.emqx_admin import EmqxAdmin, EmqxResult
from app.ingest import Ingest
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import device_secrets as device_secrets_store
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


def get_broker(request: Request) -> BrokerClient:
    """Same `request.app.state.*` dependency shape as `app/routers/
    conversations.py`'s `get_routing`/`get_location` -- this router had no
    need for the broker before docs/DEVICE_TASKS.md S4.2's `devcfg.
    push_book`/`push_cfg`, which publish `/down` directly rather than
    through `app.routing.Routing`."""
    return request.app.state.broker


def get_ingest(request: Request) -> Ingest:
    """`app/main.py` already builds one `Ingest` per app
    (`app.state.ingest`) for the webhook path (`app/routers/webhooks.py`);
    reused here (docs/DEVICE_TASKS.md S2.2's rotate-credentials route) so
    rotating a device's `hmacKey` invalidates the same in-process
    `Ingest._secret_cache` that verifies its signed envelopes, instead of a
    second `Ingest` whose cache the webhook path never sees."""
    return request.app.state.ingest


def get_emqx(request: Request) -> EmqxAdmin:
    """`app/main.py` (outside this task's `Files` list) does not build an
    `app.state.emqx_admin` the way it does `app.state.broker` -- so this
    falls back to a fresh `EmqxAdmin(settings)` per request (the same
    "safe to construct fresh per request, holds no socket" contract
    `app.emqx_admin.EmqxAdmin`'s own docstring gives) unless a test has set
    `app.state.emqx_admin` directly on the FastAPI app object after
    `create_app()` returns -- a plain attribute on Starlette's `State`, not
    something `main.py` needs to know about -- to inject a fake instead of
    a real EMQX dependency."""
    existing = getattr(request.app.state, "emqx_admin", None)
    if existing is not None:
        return existing
    return EmqxAdmin(request.app.state.settings)


def get_app_settings(request: Request) -> Settings:
    return request.app.state.settings


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
    # Carrier APN (app/apn_presets.py). Omitted/empty = carrier default.
    apn: str | None = None


class SetApnRequest(BaseModel):
    apn: str | None = None


class DeviceSetupCodeResponse(BaseModel):
    """The shared response shape for both `POST /devices` and
    `POST /devices/{id}/rotate-credentials` (docs/DEVICE_TASKS.md S2.2): the
    plaintext MQTT password/HMAC key never appear here at all -- they leave
    the relay exactly once, inside the encrypted bootstrap bundle
    (`app/devsetup.bundle`) a real device fetches over its own bootstrap
    MQTT hop (`docs/DEVICE_PLAN.md` §3.2). `setupCode` is what the admin UI
    shows the person setting the device up; `expiresAt` matches
    `app/devsetup.EXPIRY_MINUTES` from the same `now` `issue()` used."""

    device: Device
    setupCode: str
    expiresAt: datetime
    brokerPush: Literal["pushed", "manual"]
    manualAcl: list[str] | None = None


HMAC_KEY_BYTES = 32


def _bootstrap_host_and_ca(settings: Settings) -> tuple[str, str | None]:
    """docs/DEVICE_PLAN.md §3.3 / docs/V02_DESIGN.md §4.4: the broker host
    the bootstrap bundle hands the device, and the CA PEM to pin (`None`
    for an unpinned deployment). Now that `app.config.Settings` carries both
    (`broker_host`, `broker_ca_pem`), this reads them from there instead of
    `os.environ` directly -- the comment this replaced named this task as
    the one that would give it a proper home. `settings.broker_ca_pem` (a
    deployment's own Terraform-set value) wins outright; otherwise
    `ca_resolve.get_broker_ca_pem()` (docs/DEVICE_TASKS.md S2b.2's
    auto-resolve, wired in here per docs/V02_DESIGN.md §4.4's "wire that
    resolver in at last")."""
    ca = settings.broker_ca_pem or ca_resolve.get_broker_ca_pem()
    return settings.broker_host, ca


# docs/DEVICE_PLAN.md section 3.4 / section 10 H2: bit 0 of the bundle's `flags` is the
# device's `req_sig` -- "sign every publish, verify every /down". H2's answer
# is "on by default; the relay sets the flag". It MUST track the device's
# `authMode`: an `hmac` device whose bundle said flags=0 publishes unsigned,
# and `ingest.py` then drops every one of its envelopes as `bad-sig` (found
# live 2026-09-20: /status rejected, so the relay never learned the device
# speaks CBOR and kept sending it JSON it cannot parse).
_FLAG_REQ_SIG = 1


def _bootstrap_flags(auth_mode: str) -> int:
    return _FLAG_REQ_SIG if auth_mode == "hmac" else 0


def _manual_acl_lines(device_id: str) -> list[str]:
    """Human-readable mirror of `app/emqx_admin.py`'s (module-private)
    `_device_rules(device_id)` ACL -- that module is outside this task's
    `Files` list, so this is a small, by-hand-kept-in-sync duplicate for
    display only (`DeviceSetupCodeResponse.manualAcl`), never fed back into
    any broker call. Shown whenever the real push did not happen, whether
    because `BROKER_MANAGES_AUTH=0` (deliberate) or the push itself failed
    (`EmqxAdmin.ensure_device` returned `"error"`) -- either way, the admin
    still needs the exact rules to enter by hand."""
    return [
        f"publish pager/{device_id}/up",
        f"publish pager/{device_id}/status",
        f"publish pager/{device_id}/loc",
        f"subscribe pager/{device_id}/down",
    ]


def _broker_push_outcome(
    device_id: str, result: EmqxResult
) -> tuple[Literal["pushed", "manual"], list[str] | None]:
    if result == "ok":
        return "pushed", None
    return "manual", _manual_acl_lines(device_id)


@router.post("/devices", dependencies=[Depends(require_admin_write_rate_limit)])
def create_device(
    req: CreateDeviceRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
    settings: Annotated[Settings, Depends(get_app_settings)],
) -> DeviceSetupCodeResponse:
    owner_uid = _resolve_uid(req.ownerAlias)
    default_to_uid = _resolve_uid(req.defaultToAlias) if req.defaultToAlias else None
    try:
        apn = apn_presets.validate_apn(req.apn)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from None
    if devices_store.get_device(req.deviceId) is not None:
        raise HTTPException(status_code=409, detail="device already exists")

    # docs/DEVICE_PLAN.md §3.2 step 1: generate the real MQTT password and
    # HMAC key here -- this is the one moment the plaintext password exists,
    # since `devices_store.create_device` never stores it (S1.1) and
    # `device_secrets_store.create` stores only its hash/the raw key.
    password = secrets.token_urlsafe(MQTT_PASSWORD_BYTES)
    password_hash = hashlib.sha256(password.encode("utf-8")).hexdigest()
    hmac_key = secrets.token_bytes(HMAC_KEY_BYTES)

    devices_store.create_device(
        device_id=req.deviceId,
        owner_uid=owner_uid,
        label=req.label,
        mqtt_username=req.deviceId,
        mqtt_password_hash=password_hash,
        default_to_uid=default_to_uid,
        apn=apn,
    )
    device_secrets_store.create(req.deviceId, hmac_key=hmac_key, mqtt_password_hash=password_hash)
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

    # docs/DEVICE_PLAN.md §3.2 step 2: push the device's real broker
    # credential + ACL before handing out a setup code that will eventually
    # let a device connect with it.
    emqx_result = emqx.ensure_device(req.deviceId, password, req.deviceId)
    host, ca = _bootstrap_host_and_ca(settings)
    now = datetime.now(UTC)
    try:
        setup_code = devsetup.issue(
            req.deviceId,
            mqtt_password=password,
            hmac_key=hmac_key,
            host=host,
            ca_pem=ca,
            flags=_bootstrap_flags("hmac"),  # create_device()'s default authMode
            label=req.label,
            apn=apn,
            settings=settings,
            broker=broker,
            emqx=emqx,
            now=now,
        )
    except ca_resolve.PublicBaseUrlRequired as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    expires_at = now + timedelta(minutes=devsetup.EXPIRY_MINUTES)

    device = devices_store.get_device(req.deviceId)
    assert device is not None
    broker_push, manual_acl = _broker_push_outcome(req.deviceId, emqx_result)
    return DeviceSetupCodeResponse(
        device=device,
        setupCode=setup_code,
        expiresAt=expires_at,
        brokerPush=broker_push,
        manualAcl=manual_acl,
    )


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


@router.post(
    "/devices/{device_id}/rotate-credentials",
    dependencies=[Depends(require_admin_write_rate_limit)],
)
def rotate_credentials(
    device_id: str,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
    settings: Annotated[Settings, Depends(get_app_settings)],
    ingest: Annotated[Ingest, Depends(get_ingest)],
) -> DeviceSetupCodeResponse:
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")

    password = secrets.token_urlsafe(MQTT_PASSWORD_BYTES)
    password_hash = hashlib.sha256(password.encode("utf-8")).hexdigest()
    hmac_key = secrets.token_bytes(HMAC_KEY_BYTES)

    # docs/DEVICE_PLAN.md §3.5: "old password and key are dead at the
    # broker and in deviceSecrets before the new code is returned" -- delete
    # the old broker credential outright (rather than relying on
    # `ensure_device`'s idempotent password-overwrite alone) before
    # `deviceSecrets.rotate` replaces the key/hash and zeroes every replay
    # counter.
    emqx.delete_user(device.mqttUsername)
    device_secrets_store.rotate(device_id, hmac_key, password_hash)
    # docs/DEVICE_PLAN.md §2.6: "cleared on key rotation" -- this alarm and
    # its failure-time window are about the *old* key, which is now dead.
    devices_store.clear_auth_alarm(device_id)
    # S1.3 left this uncalled: without it, `app.ingest.Ingest`'s
    # in-process `_secret_cache` (docs/DEVICE_PLAN.md §2.6: "cache
    # in-process; it changes only on rotate") would keep verifying incoming
    # envelopes against the just-deleted `hmacKey` until the process
    # happened to restart.
    ingest.invalidate_secret_cache(device_id)
    devices_store.set_provision_state(device_id, "issued")

    emqx_result = emqx.ensure_device(device.mqttUsername, password, device_id)
    host, ca = _bootstrap_host_and_ca(settings)
    now = datetime.now(UTC)
    try:
        setup_code = devsetup.issue(
            device_id,
            mqtt_password=password,
            hmac_key=hmac_key,
            host=host,
            ca_pem=ca,
            flags=_bootstrap_flags(device.authMode),
            label=device.label,
            apn=device.apn,
            settings=settings,
            broker=broker,
            emqx=emqx,
            now=now,
        )
    except ca_resolve.PublicBaseUrlRequired as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    expires_at = now + timedelta(minutes=devsetup.EXPIRY_MINUTES)

    updated = devices_store.get_device(device_id)
    assert updated is not None
    broker_push, manual_acl = _broker_push_outcome(device_id, emqx_result)
    return DeviceSetupCodeResponse(
        device=updated,
        setupCode=setup_code,
        expiresAt=expires_at,
        brokerPush=broker_push,
        manualAcl=manual_acl,
    )


@router.post(
    "/devices/{device_id}/revoke", dependencies=[Depends(require_admin_write_rate_limit)]
)
def revoke_device(
    device_id: str,
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
) -> Device:
    """docs/DEVICE_PLAN.md §3.5: mounts the store function that already
    existed (`devices_store.revoke_device`) and additionally deletes the
    broker credential -- "so a stolen device cannot even connect"."""
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    emqx.delete_user(device.mqttUsername)
    return devices_store.revoke_device(device_id)


# ---------------------------------------------------------------------------
# contacts -- docs/DEVICE_PLAN.md §4.1-4.3, docs/DEVICE_TASKS.md S4.1.
#
# A `contactRequests/{deviceId}_{reqId}` row is created by `app.ingest.Ingest`
# from the device's own `/up kind:"contact_req"` (§4.2); approval/rejection
# is admin-only (§4.3: "the device owner is the student, who must not be able
# to approve their own recipients"). Every decision below bumps
# `devices/{d}.bookVersion` and calls `devcfg.push_book` (docs/DEVICE_TASKS.md
# S4.2) to publish the updated book. `app/store/contacts.py` also defines a
# `push_book` -- S4.1's placeholder no-op stub, documented there as "replace
# every call site with S4.2's real implementation." `app/store/contacts.py`
# is not in S4.2's `Files` list, so that stub is left as dead code rather
# than edited; this router (which *is* in scope, and was already the only
# caller of the stub) calls `devcfg.push_book` directly instead.
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
    broker: Annotated[BrokerClient, Depends(get_broker)],
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
    devcfg.push_book(request.deviceId, broker)
    return updated


@router.post("/contacts/{key}/reject", dependencies=[Depends(require_admin_write_rate_limit)])
def reject_contact(
    key: str,
    req: ContactRejectRequest,
    authed: Annotated[AuthedUser, Depends(require_admin)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> ContactRequest:
    request = contacts_store.get_request(key)
    if request is None:
        raise HTTPException(status_code=404, detail="no such contact request")
    if request.status != "pending":
        raise HTTPException(status_code=409, detail="contact request already decided")

    updated = contacts_store.reject(key, reason=req.reason, decided_by=authed.uid)
    contacts_store.bump_book_version(request.deviceId)
    devcfg.push_book(request.deviceId, broker)
    return updated


# ---------------------------------------------------------------------------
# cfg -- docs/DEVICE_PLAN.md §5.8, docs/DEVICE_TASKS.md S4.2.
# ---------------------------------------------------------------------------


class LockCfg(BaseModel):
    model_config = ConfigDict(extra="ignore")

    clear: bool | None = None
    # docs/DEVICE_PLAN.md §5.8: `auto_min` is stored device-side as a `u8`
    # (0-255 minutes; 0 = never).
    auto: int | None = Field(default=None, ge=0, le=255)


class PushCfgRequest(BaseModel):
    lock: LockCfg


@router.post("/devices/{device_id}/cfg", dependencies=[Depends(require_admin_write_rate_limit)])
def push_cfg(
    device_id: str,
    req: PushCfgRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> dict[str, bool]:
    if devices_store.get_device(device_id) is None:
        raise HTTPException(status_code=404, detail="no such device")
    lock = {k: v for k, v in req.lock.model_dump().items() if v is not None}
    ok = devcfg.push_cfg(device_id, lock, broker)
    return {"ok": ok}


# ---------------------------------------------------------------------------
# ca -- docs/V02_DESIGN.md §4.4, docs/V02_DESIGN.md §4.4.
# ---------------------------------------------------------------------------


class PushCaRequest(BaseModel):
    action: Literal["push", "unpin"]


@router.get("/apn-presets")
def list_apn_presets() -> list[apn_presets.ApnPreset]:
    """Carrier APN choices for the device forms (app/apn_presets.py)."""
    return apn_presets.PRESETS


@router.put("/devices/{device_id}/apn", dependencies=[Depends(require_admin_write_rate_limit)])
def set_device_apn(device_id: str, req: SetApnRequest) -> dict[str, str | None]:
    """Stores the carrier APN used for this device's setup codes and bundles.
    Nothing is pushed to the pager: an APN only changes when the pager is set
    up again (rotate credentials, then type the new code), because a wrong
    APN pushed over the air would cut the pager off with no way back."""
    if devices_store.get_device(device_id) is None:
        raise HTTPException(status_code=404, detail="no such device")
    try:
        apn = apn_presets.validate_apn(req.apn)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from None
    devices_store.set_apn(device_id, apn)
    return {"apn": apn}


@router.post("/devices/{device_id}/ca", dependencies=[Depends(require_admin_write_rate_limit)])
def push_ca(
    device_id: str,
    req: PushCaRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    settings: Annotated[Settings, Depends(get_app_settings)],
) -> dict[str, bool]:
    """`{"action": "push"}` pushes the relay's own current CA
    (`settings.broker_ca_pem` or `ca_resolve.get_broker_ca_pem()`, same
    precedence as `_bootstrap_host_and_ca`) as a `/down cfg.ca` pointer;
    `{"action": "unpin"}` pushes `cfg.ca = {url: ""}`. Like `cfg.lock`, only
    the newest unacked `cfg.ca` is re-published (`app/devcfg.py`'s
    `_PENDING_CFG_CA_FIELD`), independent of any pending `cfg.lock`."""
    if devices_store.get_device(device_id) is None:
        raise HTTPException(status_code=404, detail="no such device")
    if req.action == "unpin":
        ok = devcfg.unpin_ca(device_id, broker)
        return {"ok": ok}
    ca_pem = settings.broker_ca_pem or ca_resolve.get_broker_ca_pem()
    if not ca_pem:
        raise HTTPException(
            status_code=400, detail="no CA is configured on this relay to push"
        )
    try:
        ok = devcfg.push_ca(device_id, pem=ca_pem, broker=broker, settings=settings)
    except ca_resolve.PublicBaseUrlRequired as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return {"ok": ok}


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

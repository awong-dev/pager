"""`/api/admin/*` -- docs/SERVER_PLAN.md §5.1, §5.5; docs/FAMILIES_DESIGN.md
§4. Every route here is gated on `app.auth.require_super` (role `super` by
claim only, docs/FAMILIES_TASKS.md 1.2/5.1). Several of this router's
handlers (`_create_device_impl`,
`_rotate_credentials_impl`, `_revoke_device_impl`, `_push_cfg_impl`,
`_push_ca_impl`, `_delete_device_impl`, `_patch_user_impl`) are factored into
private module-level functions so `app/routers/family.py`'s family-scoped
routes can call the exact same setup-code/broker-push flow after their own
in-family check, instead of duplicating it (docs/FAMILIES_TASKS.md 1.3).

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

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from firebase_admin import auth as fb_auth
from pydantic import BaseModel, ConfigDict, Field

from app import apn_presets, book, ca_resolve, devcfg, devsetup
from app.auth import AuthedUser, principal_for, require_super, set_claims
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
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import messages as messages_store
from app.store import rate_limits as rate_limits_store
from app.store import settings as settings_store
from app.store import users as users_store
from app.store.allow import AllowEdge, EdgeInput
from app.store.backends import Backend
from app.store.devices import Device
from app.store.families import Family
from app.store.settings import RetentionSetting, RetentionSettings
from app.store.users import User

router = APIRouter(prefix="/api/admin", dependencies=[Depends(require_super)])

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
    authed: Annotated[AuthedUser, Depends(require_super)],
) -> None:
    """`Depends(require_super)` here is the *same* callable the router-level
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
    # docs/FAMILIES_DESIGN.md §4: super may pin a new user's family directly;
    # `None` (unset) defaults to the calling super's own family
    # (`create_user` below) -- a `person` user must never end up with
    # `familyId: null` (docs/FAMILIES_DESIGN.md §1 decision 1: "every user
    # belongs to exactly one family").
    familyId: str | None = None
    uid: str | None = None  # optional: pin a specific uid (tests, re-runs)


class PatchUserRequest(BaseModel):
    displayName: str | None = None
    email: str | None = None
    phone: str | None = None
    # docs/FAMILIES_DESIGN.md §4: super may also promote/demote to `super`
    # here (family admins do this through `PATCH /api/family/members/{uid}`,
    # which only ever accepts `admin`/`member`).
    role: Literal["super", "admin", "member"] | None = None
    # docs/FAMILIES_DESIGN.md §4: moves a user between families. Patch
    # semantics: an absent field is indistinguishable from an explicit
    # `null` here (both parse to `None`), and `_patch_user_impl` below
    # treats either as "leave familyId alone" -- so this field can move a
    # `person` user between families but can never null one out.
    familyId: str | None = None
    disabled: bool | None = None


@router.post("/users", dependencies=[Depends(require_admin_write_rate_limit)])
def create_user(
    req: CreateUserRequest, authed: Annotated[AuthedUser, Depends(require_super)]
) -> User:
    if not req.email and not req.phone:
        raise HTTPException(status_code=400, detail="email or phone is required")

    # docs/FAMILIES_DESIGN.md §1 decision 1: an omitted `familyId` defaults
    # to the calling super's own family (`fam` claim) rather than `None` --
    # a `person` user (the only `kind` this route ever creates) must never
    # end up family-less. Still `None` if the calling super has no family
    # of their own (a bootstrap/test-only edge case, not a real deployment
    # -- `app.bootstrap` always puts the super in `families/default`).
    family_id = req.familyId if req.familyId is not None else principal_for(authed).family_id
    # docs/CONTACT_REQ_DESIGN.md decision 3: a person always has a family.
    if family_id is None:
        raise HTTPException(status_code=400, detail="familyId is required")

    # Sign-in number only, never an SMS route (docs/CONTACT_REQ_DESIGN.md
    # decision 5).
    phone = req.phone
    if phone:
        try:
            phone = externals_store.normalize_phone(phone)
        except ValueError as exc:
            raise HTTPException(
                status_code=400, detail="phone must be a number like +12065550100"
            ) from exc

    kwargs: dict[str, object] = {}
    if req.uid:
        kwargs["uid"] = req.uid
    if req.email:
        kwargs["email"] = req.email
    if phone:
        kwargs["phone_number"] = phone
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
            phone=phone,
            role=req.role,
            family_id=family_id,
        )
    except (users_store.AliasTaken, users_store.InvalidAlias) as exc:
        # Roll back the just-created Auth account so a rejected alias
        # doesn't leave an orphaned, unregistered Auth user behind (which
        # the registry gate would otherwise 403 forever with no UI to fix).
        fb_auth.delete_user(auth_user.uid)
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    # docs/FAMILIES_DESIGN.md §1 decision 2: claims are the only source of
    # truth for `role`/`fam`, so every created user (not just admins) gets
    # them written, in sync with the doc just created.
    set_claims(user.uid, user.role, user.familyId)
    return user


@router.get("/users")
def list_users(family: Annotated[str | None, Query()] = None) -> list[User]:
    """docs/FAMILIES_DESIGN.md §4: `?family=` narrows the global listing to
    one family -- filtered in Python (household/deployment scale, same
    "no index needed yet" call `app/store/conversations.py`'s
    `list_groups_for_member` already makes) rather than a Firestore query,
    since `app/store/users.py` (outside this task's `Files` list) does not
    yet expose a `familyId`-filtered read."""
    users = users_store.list_users()
    if family is not None:
        users = [u for u in users if u.familyId == family]
    return users


def _patch_user_impl(
    uid: str,
    *,
    display_name: str | None,
    email: str | None,
    phone: str | None,
    role: str | None,
    family_id: str | None,
    disabled: bool | None,
    broker: BrokerClient,
) -> User:
    """Shared by `PATCH /api/admin/users/{uid}` (below, no family
    restriction) and `PATCH /api/family/members/{uid}`
    (`app/routers/family.py`, which checks the target is in its own scope
    family and never passes `family_id`) -- one setup-code-free "patch a
    user, re-issue claims if role/family changed, bump listing owners' books
    on a displayName change" flow, not duplicated per caller."""
    existing = users_store.get_user(uid)
    if existing is None:
        raise HTTPException(status_code=404, detail="no such user")
    if family_id is not None:
        # docs/FAMILIES_DESIGN.md §4: "moves a user between families" --
        # `app.store.users.update_user` (task 1.1's `Files` list, not this
        # task's) has no `family_id` parameter yet, so this writes the field
        # directly on `users/{uid}`, the same "the store doesn't expose a
        # setter for this field yet" pattern `_create_admin_asserted_
        # backend`'s `adminVerified` write already uses in this file.
        get_db().collection("users").document(uid).update({"familyId": family_id})
    user = users_store.update_user(
        uid,
        display_name=display_name,
        email=email,
        phone=phone,
        role=role,
        disabled=disabled,
    )
    if role is not None or family_id is not None:
        set_claims(uid, user.role, user.familyId)
    # docs/ADDRESS_BOOK_DESIGN.md decision 6 (extends docs/CHAT_UI_DESIGN.md
    # §1 / docs/PROTOCOL.md §3.7's displayName trigger): a `displayName`,
    # `disabled` or `familyId` change alters every book that lists this uid
    # -- the edge holders' (`_approved_contacts`) and, since the book lists
    # the whole family, every member of the old and the new family.
    name_changed = display_name is not None and display_name != existing.displayName
    disabled_changed = disabled is not None and disabled != existing.disabled
    family_changed = family_id is not None and family_id != existing.familyId
    if name_changed or disabled_changed or family_changed:
        owners = (
            book.edge_holders(uid)
            | book.family_uids(existing.familyId)
            | book.family_uids(user.familyId)
            | {uid}
        )
        book.bump_and_push(owners, broker, reason="member_patch")
    # docs/FAMILIES_DESIGN.md §3's trigger list: a displayName change
    # rewrites `participants` (and `familyIds`) in every conversation
    # this uid is a member of.
    if name_changed:
        messages_store.rewrite_participants_for_user(uid)
    return user


@router.patch("/users/{uid}", dependencies=[Depends(require_admin_write_rate_limit)])
def patch_user(
    uid: str, req: PatchUserRequest, broker: Annotated[BrokerClient, Depends(get_broker)]
) -> User:
    return _patch_user_impl(
        uid,
        display_name=req.displayName,
        email=req.email,
        phone=req.phone,
        role=req.role,
        family_id=req.familyId,
        disabled=req.disabled,
        broker=broker,
    )


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


def _message_sets_by_owner(edges: list[AllowEdge]) -> dict[str, frozenset[str]]:
    """docs/CHAT_UI_DESIGN.md §1 / docs/PROTOCOL.md §3.7: `put_allowlist`'s
    own book-bump trigger reads "the set of `(toUid)` with `message=True`" --
    the part of the allow-list that actually determines a book's `c[]`
    (`app/devcfg.py`'s `_approved_contacts` iterates
    `allow_store.allowed_recipients`, which is exactly this same filter),
    grouped by the owning `fromUid` so two snapshots (before/after
    `replace_all`) can be diffed per owner."""
    by_owner: dict[str, set[str]] = {}
    for e in edges:
        if e.message:
            by_owner.setdefault(e.fromUid, set()).add(e.toUid)
    return {uid: frozenset(to_uids) for uid, to_uids in by_owner.items()}


@router.put("/allowlist", dependencies=[Depends(require_admin_write_rate_limit)])
def put_allowlist(
    req: PutAllowlistRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    # docs/FAMILIES_DESIGN.md §4 / docs/FAMILIES_TASKS.md 2.3: stays
    # replace-all and super-only (the whole router is
    # `Depends(require_super)`) either way -- `?family=` only narrows *which
    # existing edges* the replace touches (`allow_store.replace_all`'s
    # `family_id`), it does not relax who may call this.
    family: Annotated[str | None, Query()] = None,
) -> list[AllowEdge]:
    edges = [
        EdgeInput(
            from_uid=_resolve_uid(e.fromAlias),
            to_uid=_resolve_uid(e.toAlias),
            message=e.message,
            locate=e.locate,
        )
        for e in req.entries
    ]
    # docs/PROTOCOL.md §3.7's trigger list: "an allow-list change to the
    # owner's outgoing edges" bumps and re-pushes that owner's devices'
    # books -- snapshotted *before* `replace_all` per this task's Do 7, so
    # the diff is against the pre-request state, not against `edges` (which
    # may omit an owner's untouched rows entirely under replace-all
    # semantics).
    old_message_sets = _message_sets_by_owner(allow_store.list_edges())
    try:
        result = allow_store.replace_all(edges, family_id=family)
    except allow_store.LocateCrossFamily as exc:
        # docs/FAMILIES_DESIGN.md §1 decision 5: a `locate: true` edge whose
        # ends don't share a (non-null) `familyId` is refused outright.
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    new_message_sets = _message_sets_by_owner(result)
    changed_owner_uids = {
        owner_uid
        for owner_uid in old_message_sets.keys() | new_message_sets.keys()
        if old_message_sets.get(owner_uid, frozenset())
        != new_message_sets.get(owner_uid, frozenset())
    }
    for owner_uid in changed_owner_uids:
        for device in devices_store.list_devices(owner_uid=owner_uid):
            contacts_store.bump_book_version(device.id)
            devcfg.push_book(device.id, broker)
    return result


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


def _create_device_impl(
    req: CreateDeviceRequest,
    broker: BrokerClient,
    emqx: EmqxAdmin,
    settings: Settings,
) -> DeviceSetupCodeResponse:
    """The whole setup-code flow, factored out so `app/routers/family.py`'s
    `POST /api/family/devices` can call it too, after its own in-family
    check on `req.ownerAlias` (docs/FAMILIES_TASKS.md 1.3) -- this function
    itself does not know or care whether the caller is `require_super` or
    `require_family_admin`."""
    owner_uid = _resolve_uid(req.ownerAlias)
    owner = users_store.get_user(owner_uid)
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
        # docs/FAMILIES_DESIGN.md §1 decision 1: a device's family is
        # copied from its owner at creation time, not chosen separately.
        family_id=owner.familyId if owner is not None else None,
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

    # docs/PROTOCOL.md §3.7's trigger list: "device creation or a
    # `defaultToUid` change" bumps and pushes the new device's own book.
    # `bookVersion` starts at 0, so the ordinary bootstrap-on-first-status
    # path (`app.ingest.Ingest.handle_status`) would also eventually cover
    # this -- done here anyway, per this task's Do 7, "so the book exists
    # before first status" rather than waiting for one. Best-effort: a push
    # failure (`devcfg.push_book` itself never raises) must never fail
    # device creation.
    contacts_store.bump_book_version(req.deviceId)
    devcfg.push_book(req.deviceId, broker)

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


@router.post("/devices", dependencies=[Depends(require_admin_write_rate_limit)])
def create_device(
    req: CreateDeviceRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
    settings: Annotated[Settings, Depends(get_app_settings)],
) -> DeviceSetupCodeResponse:
    return _create_device_impl(req, broker, emqx, settings)


@router.get("/devices")
def list_devices(family: Annotated[str | None, Query()] = None) -> list[Device]:
    """docs/FAMILIES_DESIGN.md §4: `?family=` narrows the global listing --
    same Python-side filter as `list_users` above, for the same reason
    (`app/store/devices.py` has no `familyId`-filtered read)."""
    devices = devices_store.list_devices()
    if family is not None:
        devices = [d for d in devices if d.familyId == family]
    return devices


def _delete_device_impl(device_id: str) -> dict[str, bool]:
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    for b in backends_store.list_backends(device.ownerUid):
        if b.kind == "pager" and b.config.get("deviceId") == device_id:
            backends_store.delete_backend(device.ownerUid, b.id)
    devices_store.delete_device(device_id)
    return {"ok": True}


@router.delete("/devices/{device_id}", dependencies=[Depends(require_admin_write_rate_limit)])
def delete_device(device_id: str) -> dict[str, bool]:
    return _delete_device_impl(device_id)


def _rotate_credentials_impl(
    device_id: str,
    broker: BrokerClient,
    emqx: EmqxAdmin,
    settings: Settings,
    ingest: Ingest,
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
    return _rotate_credentials_impl(device_id, broker, emqx, settings, ingest)


def _revoke_device_impl(device_id: str, emqx: EmqxAdmin) -> Device:
    """docs/DEVICE_PLAN.md §3.5: mounts the store function that already
    existed (`devices_store.revoke_device`) and additionally deletes the
    broker credential -- "so a stolen device cannot even connect"."""
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    emqx.delete_user(device.mqttUsername)
    return devices_store.revoke_device(device_id)


@router.post(
    "/devices/{device_id}/revoke", dependencies=[Depends(require_admin_write_rate_limit)]
)
def revoke_device(
    device_id: str,
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
) -> Device:
    return _revoke_device_impl(device_id, emqx)


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


# Contact approval lives in routers/family.py, docs/CONTACT_REQ_DESIGN.md.


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


def _push_cfg_impl(device_id: str, req: PushCfgRequest, broker: BrokerClient) -> dict[str, bool]:
    if devices_store.get_device(device_id) is None:
        raise HTTPException(status_code=404, detail="no such device")
    lock = {k: v for k, v in req.lock.model_dump().items() if v is not None}
    ok = devcfg.push_cfg(device_id, lock, broker)
    return {"ok": ok}


@router.post("/devices/{device_id}/cfg", dependencies=[Depends(require_admin_write_rate_limit)])
def push_cfg(
    device_id: str,
    req: PushCfgRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> dict[str, bool]:
    return _push_cfg_impl(device_id, req, broker)


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


def _push_ca_impl(
    device_id: str, req: PushCaRequest, broker: BrokerClient, settings: Settings
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


@router.post("/devices/{device_id}/ca", dependencies=[Depends(require_admin_write_rate_limit)])
def push_ca(
    device_id: str,
    req: PushCaRequest,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    settings: Annotated[Settings, Depends(get_app_settings)],
) -> dict[str, bool]:
    return _push_ca_impl(device_id, req, broker, settings)


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


# ---------------------------------------------------------------------------
# families -- docs/FAMILIES_DESIGN.md §4. Super-only: creating a family and
# renaming one/setting its SMS number. Everything scoped *to* a family (the
# member/device/group CRUD) lives in `app/routers/family.py`, reachable by a
# family's own `admin` too via `require_family_admin`.
# ---------------------------------------------------------------------------


class CreateFamilyRequest(BaseModel):
    name: str


class PatchFamilyRequest(BaseModel):
    name: str | None = None
    smsNumber: str | None = None


@router.get("/families")
def list_families() -> list[Family]:
    return families_store.list_families()


@router.post("/families", dependencies=[Depends(require_admin_write_rate_limit)])
def create_family(
    req: CreateFamilyRequest, authed: Annotated[AuthedUser, Depends(require_super)]
) -> Family:
    return families_store.create_family(name=req.name, created_by=authed.uid)


@router.patch("/families/{fid}", dependencies=[Depends(require_admin_write_rate_limit)])
def patch_family(fid: str, req: PatchFamilyRequest) -> Family:
    name = req.name
    if name is not None:
        try:
            name = families_store.validate_family_name(name)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
    try:
        return families_store.update_family(fid, name=name, sms_number=req.smsNumber)
    except KeyError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc

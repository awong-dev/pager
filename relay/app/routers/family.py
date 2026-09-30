"""`/api/family/*` -- docs/FAMILIES_DESIGN.md §4. Every route here is gated
on `app.auth.require_family_admin`: role `admin` acting on their own family,
or role `super` naming any family via `?family=`/`X-Family` (`(Principal,
family_id)`, the family in scope for the whole request).

Member and group creation/patch reuse `app/routers/admin.py`'s user-creation
and displayName/role/family-claims flow (`_patch_user_impl`); device CRUD
reuses its setup-code flow (`_create_device_impl`,
`_rotate_credentials_impl`, `_revoke_device_impl`, `_push_cfg_impl`,
`_push_ca_impl`, `_delete_device_impl`) after this router's own in-family
check -- one implementation, not a copy per surface
(docs/FAMILIES_TASKS.md 1.3).
"""

from __future__ import annotations

from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException, Request
from firebase_admin import auth as fb_auth
from google.api_core.exceptions import AlreadyExists
from pydantic import BaseModel, Field

from app.auth import Principal, require_family_admin, set_claims
from app.broker import BrokerClient
from app.config import Settings
from app.emqx_admin import EmqxAdmin
from app.ingest import Ingest
from app.routers import admin as admin_router
from app.routers import conversations as conversations_router
from app.store import allow as allow_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import families as families_store
from app.store import rate_limits as rate_limits_store
from app.store import users as users_store
from app.store.devices import Device
from app.store.families import Family
from app.store.users import InvalidAlias, User

router = APIRouter(prefix="/api/family")

# `(Principal, family_id)` -- the family in scope for this request, from
# `require_family_admin` (an `admin`'s own family, or the family a `super`
# named via `?family=`/`X-Family`).
FamilyScope = Annotated[tuple[Principal, str], Depends(require_family_admin)]


def require_family_write_rate_limit(scope: FamilyScope) -> None:
    """Same coarse, per-uid fixed-window counter `app/routers/admin.py`'s
    `require_admin_write_rate_limit` uses (`"admin:{uid}"`, shared with the
    admin surface on purpose -- one person's overall write rate is what's
    bounded) -- reusing its env-configured limit/window rather than adding a
    second knob. `Depends(require_family_admin)` here is the same call the
    router-level-equivalent per-route dependency already makes for this
    request, so FastAPI's per-request dependency cache returns the cached
    result rather than re-verifying the bearer token a second time."""
    principal, _ = scope
    limit, window_s = admin_router._admin_write_rate_limit()
    if not rate_limits_store.check_and_increment(
        f"admin:{principal.uid}", limit=limit, window_s=window_s
    ):
        raise HTTPException(status_code=429, detail="admin rate limit exceeded; try again later")


def get_broker(request: Request) -> BrokerClient:
    return request.app.state.broker


def get_ingest(request: Request) -> Ingest:
    return request.app.state.ingest


def get_emqx(request: Request) -> EmqxAdmin:
    """Same fallback-to-a-fresh-instance shape as `app/routers/admin.py`'s
    own `get_emqx` (this router shares `app.state.emqx_admin` with it when a
    test has set one)."""
    existing = getattr(request.app.state, "emqx_admin", None)
    if existing is not None:
        return existing
    return EmqxAdmin(request.app.state.settings)


def get_app_settings(request: Request) -> Settings:
    return request.app.state.settings


# ---------------------------------------------------------------------------
# family + members
# ---------------------------------------------------------------------------


class FamilyOut(Family):
    memberCount: int
    deviceCount: int


@router.get("")
def get_family(scope: FamilyScope) -> FamilyOut:
    _, family_id = scope
    family = families_store.get_family(family_id)
    if family is None:
        raise HTTPException(status_code=404, detail="no such family")
    member_count = sum(1 for u in users_store.list_users() if u.familyId == family_id)
    device_count = sum(1 for d in devices_store.list_devices() if d.familyId == family_id)
    return FamilyOut(**family.model_dump(), memberCount=member_count, deviceCount=device_count)


class MembersOut(BaseModel):
    members: list[User]


@router.get("/members")
def list_members(scope: FamilyScope) -> MembersOut:
    _, family_id = scope
    return MembersOut(members=[u for u in users_store.list_users() if u.familyId == family_id])


class CreateMemberRequest(BaseModel):
    alias: str
    displayName: str
    email: str | None = None
    phone: str | None = None
    role: Literal["admin", "member"] = "member"


@router.post("/members", dependencies=[Depends(require_family_write_rate_limit)])
def create_member(req: CreateMemberRequest, scope: FamilyScope) -> User:
    """The same Auth-account-plus-registry-row flow as `POST
    /api/admin/users` (`app/routers/admin.py`'s `create_user`), with the
    scope family baked in rather than accepted as input."""
    _, family_id = scope
    if not req.email and not req.phone:
        raise HTTPException(status_code=400, detail="email or phone is required")

    kwargs: dict[str, object] = {}
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
            family_id=family_id,
        )
    except (users_store.AliasTaken, users_store.InvalidAlias) as exc:
        fb_auth.delete_user(auth_user.uid)
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    set_claims(user.uid, user.role, user.familyId)
    return user


class PatchMemberRequest(BaseModel):
    displayName: str | None = None
    role: Literal["admin", "member"] | None = None
    disabled: bool | None = None


@router.patch("/members/{uid}", dependencies=[Depends(require_family_write_rate_limit)])
def patch_member(
    uid: str,
    req: PatchMemberRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> User:
    _, family_id = scope
    existing = users_store.get_user(uid)
    if existing is None or existing.familyId != family_id:
        raise HTTPException(status_code=404, detail="no such user in this family")
    return admin_router._patch_user_impl(
        uid,
        display_name=req.displayName,
        email=None,
        phone=None,
        role=req.role,
        family_id=None,
        disabled=req.disabled,
        broker=broker,
    )


# ---------------------------------------------------------------------------
# devices -- delegate to app/routers/admin.py's setup-code flow after an
# in-family check.
# ---------------------------------------------------------------------------


def _require_family_owner(owner_alias: str, family_id: str) -> str:
    owner_uid = users_store.get_uid_for_alias(owner_alias)
    if owner_uid is None:
        raise HTTPException(status_code=400, detail=f"unknown alias: {owner_alias!r}")
    owner = users_store.get_user(owner_uid)
    if owner is None or owner.familyId != family_id:
        raise HTTPException(status_code=403, detail="owner is not a member of this family")
    return owner_uid


def _require_family_device(device_id: str, family_id: str) -> Device:
    device = devices_store.get_device(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="no such device")
    if device.familyId != family_id:
        raise HTTPException(status_code=403, detail="device belongs to another family")
    return device


@router.get("/devices")
def list_devices(scope: FamilyScope) -> list[Device]:
    _, family_id = scope
    return [d for d in devices_store.list_devices() if d.familyId == family_id]


@router.post("/devices", dependencies=[Depends(require_family_write_rate_limit)])
def create_device(
    req: admin_router.CreateDeviceRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
    settings: Annotated[Settings, Depends(get_app_settings)],
) -> admin_router.DeviceSetupCodeResponse:
    _, family_id = scope
    _require_family_owner(req.ownerAlias, family_id)
    return admin_router._create_device_impl(req, broker, emqx, settings)


@router.post(
    "/devices/{device_id}/rotate-credentials",
    dependencies=[Depends(require_family_write_rate_limit)],
)
def rotate_credentials(
    device_id: str,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
    settings: Annotated[Settings, Depends(get_app_settings)],
    ingest: Annotated[Ingest, Depends(get_ingest)],
) -> admin_router.DeviceSetupCodeResponse:
    _, family_id = scope
    _require_family_device(device_id, family_id)
    return admin_router._rotate_credentials_impl(device_id, broker, emqx, settings, ingest)


@router.post(
    "/devices/{device_id}/revoke", dependencies=[Depends(require_family_write_rate_limit)]
)
def revoke_device(
    device_id: str,
    scope: FamilyScope,
    emqx: Annotated[EmqxAdmin, Depends(get_emqx)],
) -> Device:
    _, family_id = scope
    _require_family_device(device_id, family_id)
    return admin_router._revoke_device_impl(device_id, emqx)


@router.post("/devices/{device_id}/cfg", dependencies=[Depends(require_family_write_rate_limit)])
def push_cfg(
    device_id: str,
    req: admin_router.PushCfgRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> dict[str, bool]:
    _, family_id = scope
    _require_family_device(device_id, family_id)
    return admin_router._push_cfg_impl(device_id, req, broker)


@router.post("/devices/{device_id}/ca", dependencies=[Depends(require_family_write_rate_limit)])
def push_ca(
    device_id: str,
    req: admin_router.PushCaRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    settings: Annotated[Settings, Depends(get_app_settings)],
) -> dict[str, bool]:
    _, family_id = scope
    _require_family_device(device_id, family_id)
    return admin_router._push_ca_impl(device_id, req, broker, settings)


@router.delete("/devices/{device_id}", dependencies=[Depends(require_family_write_rate_limit)])
def delete_device(device_id: str, scope: FamilyScope) -> dict[str, bool]:
    _, family_id = scope
    _require_family_device(device_id, family_id)
    return admin_router._delete_device_impl(device_id)


# ---------------------------------------------------------------------------
# groups -- docs/GROUP_CHAT_DESIGN.md §3, restricted to family-or-edge peers.
# ---------------------------------------------------------------------------


class CreateGroupRequest(BaseModel):
    name: str
    alias: str
    memberUids: list[str] = Field(min_length=2)


class CreateGroupResponse(BaseModel):
    convKey: str
    alias: str


@router.post("/groups", status_code=201, dependencies=[Depends(require_family_write_rate_limit)])
def create_group(
    req: CreateGroupRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> CreateGroupResponse:
    principal, family_id = scope
    member_uids = sorted(set(req.memberUids))
    for uid in member_uids:
        user = users_store.get_user(uid)
        if user is None:
            raise HTTPException(status_code=404, detail=f"no such user: {uid!r}")
        if uid == principal.uid:
            continue
        in_family = user.familyId == family_id
        has_edge = allow_store.is_message_allowed(
            principal.uid, uid
        ) or allow_store.is_message_allowed(uid, principal.uid)
        if not in_family and not has_edge:
            raise HTTPException(
                status_code=403,
                detail=(
                    f"{uid!r} is not in this family and has no message edge "
                    "with the creator"
                ),
            )

    try:
        conv = conversations_store.create_group(
            name=req.name, alias=req.alias, member_uids=member_uids, created_by=principal.uid
        )
    except InvalidAlias as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except AlreadyExists as exc:
        raise HTTPException(status_code=409, detail="alias already taken") from exc

    # docs/GROUP_CHAT_DESIGN.md decision 2: creation (unlike join, task 1.3)
    # still creates full `message`+`locate` edges between every member pair.
    conversations_router._create_missing_allow_edges(conv.uids)
    conversations_router._push_book_to_members(conv.uids, broker)
    return CreateGroupResponse(convKey=conv.convKey, alias=req.alias)

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

import logging
from datetime import datetime
from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException, Request
from firebase_admin import auth as fb_auth
from google.api_core.exceptions import AlreadyExists
from pydantic import BaseModel, ConfigDict, Field

from app import book, devcfg, sms_compliance
from app import policy as policy_module
from app.auth import Principal, require_family_admin, set_claims
from app.backends import sms_twilio
from app.book import rederive_family_sms_contacts, rederive_sms_contacts
from app.broker import BrokerClient
from app.config import Settings
from app.db.firestore import get_db
from app.emqx_admin import EmqxAdmin
from app.ingest import Ingest
from app.notify import sms as sms_client
from app.routers import admin as admin_router
from app.routers import conversations as conversations_router
from app.routing import Routing
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import bridge_conversations
from app.store import bridges as bridges_store
from app.store import contacts as contacts_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import held_chat as held_chat_store
from app.store import held_sms as held_sms_store
from app.store import rate_limits as rate_limits_store
from app.store import sms_consent as sms_consent_store
from app.store import users as users_store
from app.store.alerts import Alert
from app.store.devices import Device
from app.store.families import Family
from app.store.users import InvalidAlias, Policy, User

logger = logging.getLogger("relay.family")

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


def _family_out(family_id: str) -> FamilyOut:
    family = families_store.get_family(family_id)
    if family is None:
        raise HTTPException(status_code=404, detail="no such family")
    member_count = sum(1 for u in users_store.list_users() if u.familyId == family_id)
    device_count = sum(1 for d in devices_store.list_devices() if d.familyId == family_id)
    return FamilyOut(**family.model_dump(), memberCount=member_count, deviceCount=device_count)


@router.get("")
def get_family(scope: FamilyScope) -> FamilyOut:
    _, family_id = scope
    return _family_out(family_id)


class PatchFamilyRequest(BaseModel):
    name: str


@router.patch("", dependencies=[Depends(require_family_write_rate_limit)])
def patch_family(req: PatchFamilyRequest, scope: FamilyScope) -> FamilyOut:
    """docs/CONTACT_REQ_DESIGN.md decision 6: a family admin renames their
    own family. The name appears only on the web: no book bump, no push."""
    _, family_id = scope
    try:
        name = families_store.validate_family_name(req.name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    try:
        families_store.update_family(family_id, name=name)
    except KeyError as exc:
        raise HTTPException(status_code=404, detail="no such family") from exc
    return _family_out(family_id)


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
def create_member(
    req: CreateMemberRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> User:
    """The same Auth-account-plus-registry-row flow as `POST
    /api/admin/users` (`app/routers/admin.py`'s `create_user`), with the
    scope family baked in rather than accepted as input."""
    _, family_id = scope
    if not req.email and not req.phone:
        raise HTTPException(status_code=400, detail="email or phone is required")

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
        fb_auth.delete_user(auth_user.uid)
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    set_claims(user.uid, user.role, user.familyId)
    # docs/ADDRESS_BOOK_DESIGN.md decision 6: the book lists the whole
    # family, so every member's book gains the newcomer.
    book.bump_and_push(book.family_uids(family_id), broker, reason="member_create")
    return user


class PatchMemberRequest(BaseModel):
    displayName: str | None = None
    role: Literal["admin", "member"] | None = None
    disabled: bool | None = None
    # docs/FAMILIES_DESIGN.md §2 / docs/FAMILIES_TASKS.md 3.1: the member's
    # two policy pickers, validated against `app.policy.OUT`/`IN` below.
    policy: Policy | None = None
    # docs/RELAY_SMS_DESIGN.md decision 1: absent = leave alone, null = clear.
    smsNumber: str | None = None


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

    if req.policy is not None:
        if req.policy.out not in policy_module.OUT:
            raise HTTPException(status_code=400, detail=f"invalid policy.out: {req.policy.out!r}")
        if req.policy.in_ not in policy_module.IN:
            raise HTTPException(status_code=400, detail=f"invalid policy.in: {req.policy.in_!r}")
        # `app/store/users.py` (outside this task's `Files` list) has no
        # `policy` setter yet -- same "write the field directly" pattern
        # `app/routers/admin.py`'s `_patch_user_impl` already uses for
        # `familyId`. Written before `_patch_user_impl` below so the `User`
        # it returns (a fresh `get_user`) reflects it.
        get_db().collection("users").document(uid).update(
            {"policy": {"out": req.policy.out, "in": req.policy.in_}}
        )

    policy_changed = req.policy is not None and req.policy != existing.policy
    result = admin_router._patch_user_impl(
        uid,
        display_name=req.displayName,
        email=None,
        phone=None,
        role=req.role,
        family_id=None,
        disabled=req.disabled,
        broker=broker,
        sms_number=req.smsNumber if "smsNumber" in req.model_fields_set else users_store.UNSET,
    )
    if policy_changed:
        # docs/ADDRESS_BOOK_DESIGN.md decision 6: a policy change flips
        # `sendable` for entries in this member's book and in every
        # sibling's book (the recipient's `in` rule).
        book.bump_and_push(book.family_uids(family_id) | {uid}, broker, reason="policy")
        # An open/any_sms outbound policy implies every family contact on
        # this member's `cfg.sms` (docs/V02_DESIGN.md §6).
        rederive_sms_contacts(uid, broker)
    return result


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
        if user.kind == "external":
            raise HTTPException(status_code=400, detail="an SMS contact cannot join a group")
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

    # docs/GROUP_CHAT_DESIGN.md decision 2's mutual edges, message-only --
    # docs/FAMILIES_TASKS.md 3.2 addition (b) supersedes task 1.3's own note
    # here ("creation, unlike join, still creates ... `locate` edges"):
    # creation now shares join's `_create_missing_message_edges`, never
    # `locate`, so a member spanning two families (the out-of-family edge
    # peer case below) can join a group without tripping
    # `allow_store.check_locate_family`.
    conversations_router._create_missing_message_edges(conv.uids)
    conversations_router._push_book_to_members(conv.uids, broker)
    return CreateGroupResponse(convKey=conv.convKey, alias=req.alias)


# ---------------------------------------------------------------------------
# approved people/contacts -- docs/FAMILIES_DESIGN.md §1 decision 7, §4
# `/approved`; docs/FAMILIES_TASKS.md 3.2.
#
# "Approved people" and "approved contacts" *are* the member's own outgoing
# `allow` edges (decision 7) -- this endpoint is the single writer of that
# outgoing set AND its full replacement: whatever the PUT does not list,
# people and contacts alike, loses its edge (8 Oct 2026 owner report: the
# web only sends checked people, so keeping unlisted person edges meant
# unchecking one never stuck). One direction only (`uid` -> peer/contact):
# it does not touch the peer's own outgoing edge back to `uid`: if the
# peer's own inbound policy needs an edge too, the peer (or their family
# admin) grants it through their own `/approved` PUT, same as any other
# policy-gated pair (`app/policy.py`'s `check`). Contacts are *picked* (by
# uid) from the family's SMS contacts (`/api/family/contacts`); this route
# never creates one.
# ---------------------------------------------------------------------------


class ApprovedPerson(BaseModel):
    alias: str
    message: bool = True
    locate: bool = False


class ApprovedContact(BaseModel):
    uid: str
    message: bool = True


class PutApprovedRequest(BaseModel):
    # `extra="forbid"`: a stale client still sending `numbers` gets a 422
    # instead of being silently ignored.
    model_config = ConfigDict(extra="forbid")

    people: list[ApprovedPerson] = []
    contacts: list[ApprovedContact] = []


class ApprovedOut(BaseModel):
    people: list[ApprovedPerson]
    contacts: list[ApprovedContact]


def _require_family_member(uid: str, family_id: str) -> User:
    member = users_store.get_user(uid)
    if member is None or member.familyId != family_id:
        raise HTTPException(status_code=404, detail="no such user in this family")
    return member


def _approved_out(uid: str, family_id: str) -> ApprovedOut:
    """The member's outgoing edges as the web lists them (shared by GET and
    the PUT response so the two can never disagree)."""
    people: list[ApprovedPerson] = []
    contacts: list[ApprovedContact] = []
    for edge in allow_store.list_edges():
        if edge.fromUid != uid:
            continue
        peer = users_store.get_user(edge.toUid)
        if peer is None:
            continue
        if peer.kind == "external":
            # A Google Chat subscription edge is managed under Google Chat
            # (docs/BRIDGE_PHONE_DESIGN.md), never through the contact editor.
            if peer.ownerFamilyId == family_id and not peer.chat:
                contacts.append(ApprovedContact(uid=peer.uid, message=edge.message))
        else:
            people.append(
                ApprovedPerson(alias=peer.alias, message=edge.message, locate=edge.locate)
            )
    people.sort(key=lambda p: p.alias)
    contacts.sort(key=lambda c: c.uid)
    return ApprovedOut(people=people, contacts=contacts)


@router.get("/members/{uid}/approved")
def get_approved(uid: str, scope: FamilyScope) -> ApprovedOut:
    _, family_id = scope
    _require_family_member(uid, family_id)
    return _approved_out(uid, family_id)


@router.put("/members/{uid}/approved", dependencies=[Depends(require_family_write_rate_limit)])
def put_approved(
    uid: str,
    req: PutApprovedRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> ApprovedOut:
    _, family_id = scope
    _require_family_member(uid, family_id)

    # People: resolve and check-before-write, per person, so a bad alias or
    # a cross-family `locate: true` 400s before any edge is touched.
    resolved_people: list[tuple[str, ApprovedPerson]] = []
    for p in req.people:
        peer_uid = users_store.get_uid_for_alias(p.alias)
        if peer_uid is None:
            raise HTTPException(status_code=400, detail=f"unknown alias: {p.alias!r}")
        if peer_uid == uid:
            raise HTTPException(status_code=400, detail="cannot approve yourself")
        try:
            allow_store.check_locate_family(uid, peer_uid, p.locate)
        except allow_store.LocateCrossFamily as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        resolved_people.append((peer_uid, p))

    # Contacts: every uid must be one of this family's SMS contacts, checked
    # before any write. Always message-only (an external never gets `locate`).
    for c in req.contacts:
        contact = users_store.get_user(c.uid)
        if (
            contact is None
            or contact.kind != "external"
            or contact.ownerFamilyId != family_id
            or contact.chat
        ):
            raise HTTPException(status_code=404, detail=f"no such contact: {c.uid}")

    for peer_uid, p in resolved_people:
        allow_store.set_edge(uid, peer_uid, message=p.message, locate=p.locate)
    people_changed = bool(resolved_people)

    # Full replacement: any outgoing edge of `uid` not listed is deleted,
    # person or external (see the comment above). One scan, one `get_user`
    # per peer.
    kept_people_uids = {peer_uid for peer_uid, _ in resolved_people}
    kept_contact_uids = {c.uid for c in req.contacts}
    peers: dict[str, User | None] = {}
    for edge in allow_store.list_edges():
        if edge.fromUid != uid:
            continue
        if edge.toUid not in peers:
            peers[edge.toUid] = users_store.get_user(edge.toUid)
        peer = peers[edge.toUid]
        if peer is None:
            continue
        if peer.kind == "external":
            if peer.chat:
                continue  # the subscribe edge belongs to Google Chat, not this editor
            if edge.toUid not in kept_contact_uids:
                allow_store.delete_edge(uid, edge.toUid)
        elif edge.toUid not in kept_people_uids:
            allow_store.delete_edge(uid, edge.toUid)
            people_changed = True

    for c in req.contacts:
        allow_store.set_edge(uid, c.uid, message=c.message, locate=False)

    rederive_family_sms_contacts(family_id, broker)
    if people_changed:
        # The book's own `c[]` projects the member's people edges
        # (`app/devcfg.py`'s `_approved_contacts`; externals never reach it),
        # which just changed for every device `uid` owns -- same
        # bump-then-push pair `PUT /api/admin/allowlist` uses.
        for device in devices_store.list_devices(owner_uid=uid):
            contacts_store.bump_book_version(device.id)
            devcfg.push_book(device.id, broker)

    return _approved_out(uid, family_id)


# ---------------------------------------------------------------------------
# contacts -- docs/FAMILIES_DESIGN.md §4 `/api/family/contacts`, §5.4
# Contacts page: the family's externals, independent of which member(s)
# approved them.
# ---------------------------------------------------------------------------


class ContactOut(BaseModel):
    uid: str
    alias: str
    phone: str | None
    displayName: str
    approvedFor: list[str]
    # Members who hold the contact through their outbound policy alone (open /
    # any_sms, no explicit edge either way) -- docs/V02_DESIGN.md §6.
    impliedFor: list[str] = []


def _name_taken(family_id: str, exc: externals_store.ContactNameTaken) -> HTTPException:
    logger.info("contact name taken family=%s", family_id)
    return HTTPException(status_code=409, detail=str(exc))


def _list_contacts(family_id: str) -> list[ContactOut]:
    members = [u for u in users_store.list_users() if u.familyId == family_id]
    member_uids = {u.uid for u in members}
    approved_for: dict[str, set[str]] = {}
    has_edge: dict[str, set[str]] = {}
    for edge in allow_store.list_edges():
        if edge.fromUid in member_uids:
            has_edge.setdefault(edge.toUid, set()).add(edge.fromUid)
            if edge.message:
                approved_for.setdefault(edge.toUid, set()).add(edge.fromUid)
    implying = [
        u.uid
        for u in members
        if u.kind == "person"
        and not u.disabled
        and policy_module.rule(u.policy.out, "external") == "any"
    ]

    out = [
        ContactOut(
            uid=ext.uid,
            alias=ext.alias,
            phone=ext.phone,
            displayName=ext.displayName,
            approvedFor=sorted(approved_for.get(ext.uid, set())),
            impliedFor=sorted(set(implying) - has_edge.get(ext.uid, set())),
        )
        for ext in externals_store.list_family_contacts(family_id)
        if not ext.chat
    ]
    out.sort(key=lambda c: c.displayName.casefold())
    return out


@router.get("/contacts")
def list_contacts(scope: FamilyScope) -> list[ContactOut]:
    _, family_id = scope
    return _list_contacts(family_id)


class CreateContactRequest(BaseModel):
    phone: str
    name: str = Field(min_length=1)


def _contact_out(family_id: str, external: User) -> ContactOut:
    """The contact as `GET /contacts` reports it (true approvedFor/impliedFor)."""
    return next(c for c in _list_contacts(family_id) if c.uid == external.uid)


def _consent_by_admin(family_id: str, e164: str, from_number: str | None) -> None:
    """docs/RELAY_SMS_DESIGN.md decision 11: an admin adding or approving a
    number is its opt-in. On the first opt-in, send the welcome from
    `from_number` (default: the family's first person, by alias, with a relay
    number); with none, warn and leave the row `opted_in`."""
    if not sms_consent_store.mark_opted_in(e164, source="admin"):
        return
    if from_number is None:
        members = sorted(
            (
                u
                for u in users_store.list_users()
                if u.kind == "person" and u.familyId == family_id and u.smsNumber
            ),
            key=lambda u: u.alias,
        )
        from_number = members[0].smsNumber if members else None
    if from_number is None:
        logger.warning("sms welcome skipped: no member number")
        return
    # docs/BRIDGE_PHONE_DESIGN.md O3 (+ O1 revised): a member texting from a
    # bridge phone's SIM or Voice number needs no disclosure.
    if bridges_store.get_by_sms_number(from_number) is not None:
        logger.info("sms welcome skipped: bridge number")
        return
    sms_client.send_sms(e164, sms_compliance.welcome(), from_number=from_number)


@router.post(
    "/contacts", status_code=201, dependencies=[Depends(require_family_write_rate_limit)]
)
def create_contact(
    req: CreateContactRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> ContactOut:
    """Creates an external with no explicit edges -- docs/FAMILIES_TASKS.md
    3.2: "POST creates one without edges." Members whose outbound policy
    allows any number get it on their `cfg.sms` at once (implied approval);
    for the others, an approval is picked in `/approved`. 409 if another
    contact of the family already has the name (the pager matches by name)."""
    _, family_id = scope
    try:
        external = externals_store.get_or_create(family_id, req.phone, req.name)
    except externals_store.ContactNameTaken as exc:
        raise _name_taken(family_id, exc) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    _consent_by_admin(family_id, external.phone or req.phone, None)
    rederive_family_sms_contacts(family_id, broker)
    return _contact_out(family_id, external)


class PatchContactRequest(BaseModel):
    name: str = Field(min_length=1)


def _require_family_contact(uid: str, family_id: str) -> User:
    external = users_store.get_user(uid)
    # docs/CONTACT_REQ_DESIGN.md decision 7: a contact belongs to one family.
    if external is None or external.kind != "external" or external.ownerFamilyId != family_id:
        raise HTTPException(status_code=404, detail="no such contact")
    return external


@router.patch("/contacts/{uid}", dependencies=[Depends(require_family_write_rate_limit)])
def patch_contact(
    uid: str,
    req: PatchContactRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> ContactOut:
    _, family_id = scope
    external = _require_family_contact(uid, family_id)
    try:
        updated = externals_store.rename(family_id, external.uid, req.name)
    except externals_store.ContactNameTaken as exc:
        raise _name_taken(family_id, exc) from exc
    # Explicit and implied holders alike carry the name on `cfg.sms`.
    rederive_family_sms_contacts(family_id, broker)
    return _contact_out(family_id, updated)


@router.delete("/contacts/{uid}", dependencies=[Depends(require_family_write_rate_limit)])
def delete_contact(
    uid: str,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> dict[str, bool]:
    """Deletes the contact: every `allow` edge to/from it, its name
    reservation, and the user plus alias. Messages stay as history."""
    _, family_id = scope
    _require_family_contact(uid, family_id)
    externals_store.delete(family_id, uid)
    rederive_family_sms_contacts(family_id, broker)
    return {"ok": True}


# ---------------------------------------------------------------------------
# alerts -- docs/FAMILIES_DESIGN.md §4 `/api/family/alerts`, §6 "Alert
# creation", §5.4 Alerts; docs/FAMILIES_TASKS.md 4.1.
# ---------------------------------------------------------------------------


class AlertsOut(BaseModel):
    alerts: list[Alert]


@router.get("/alerts")
def list_alerts(scope: FamilyScope, status: Literal["open", "all"] = "open") -> AlertsOut:
    _, family_id = scope
    return AlertsOut(alerts=alerts_store.list_alerts(family_id, status))


def _require_open_family_alert(alert_id: str, family_id: str) -> Alert:
    alert = alerts_store.get(family_id, alert_id)
    if alert is None:
        raise HTTPException(status_code=404, detail="no such alert")
    if alert.status != "open":
        raise HTTPException(status_code=409, detail="alert already decided")
    return alert


class ApproveAlertRequest(BaseModel):
    """One Optional-everything body for all three alert kinds (docs/
    FAMILIES_TASKS.md 4.1's web contract): `sms_unknown` sends `{name,
    forAlias}`, `new_conversation` sends `{}`, `contact_request` sends
    `{mode, alias}`. Dispatch is on the *stored* alert's `kind`, not on
    which of these fields the client happened to send."""

    name: str | None = None
    forAlias: str | None = None
    # `contact_request` approval is one click (docs/CONTACT_REQ_DESIGN.md
    # decision 2): `mode`/`alias` are ignored, except `mode:"create"`, which
    # is refused -- approval never creates a person.
    mode: Literal["link", "create"] | None = None
    alias: str | None = None


class AlertDecision(BaseModel):
    """`POST /alerts/{id}/approve` for an `sms_unknown` alert: the decided
    alert plus how many held texts were delivered / left held."""

    alert: Alert
    delivered: int = 0
    undelivered: int = 0


def _approve_sms_unknown(
    alert: Alert,
    req: ApproveAlertRequest,
    family_id: str,
    broker: BrokerClient,
    routing: Routing,
) -> tuple[int, int]:
    """docs/RELAY_SMS_DESIGN.md decision 6, steps (a)-(e) in order, each
    idempotent so a retry after a crash finishes the job. Returns
    `(delivered, undelivered)`; the caller does (f), `decide`."""
    if not req.name:
        raise HTTPException(status_code=400, detail="name is required")
    if alert.peerPhone is None:
        raise HTTPException(status_code=400, detail="alert has no phone number")

    target_uid = alert.subjectUid
    if target_uid is None:
        if not req.forAlias:
            raise HTTPException(status_code=400, detail="forAlias is required for this alert")
        target_uid = users_store.get_uid_for_alias(req.forAlias)
        if target_uid is None:
            raise HTTPException(status_code=400, detail=f"unknown alias: {req.forAlias!r}")
    target = users_store.get_user(target_uid)
    if target is None or target.familyId != family_id:
        raise HTTPException(status_code=403, detail="target is not a member of this family")

    held = held_sms_store.list_held(family_id, alert.peerPhone, target_uid, status="held")
    # (a) nothing is written if the member's inbound policy refuses numbers.
    if held and policy_module.rule(target.policy.in_, "external") == "none":
        raise HTTPException(
            status_code=409,
            detail=(
                f"@{target.alias}'s inbound policy does not allow numbers; "
                "change it under People"
            ),
        )

    # (b) the contact.
    try:
        external = externals_store.get_or_create(family_id, alert.peerPhone, req.name)
    except externals_store.ContactNameTaken as exc:
        raise _name_taken(family_id, exc) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    # Admin consent before the backlog goes out, so the pager's reply to it is
    # not suppressed (decision 11).
    _consent_by_admin(family_id, external.phone or alert.peerPhone, target.smsNumber)
    # (c) the edge.
    allow_store.set_edge(target_uid, external.uid, message=True, locate=False)
    allow_store.recompute_locatable_by_for_owner(external.uid)
    # (d) the contact reaches the family's pagers; the member's book changes.
    rederive_family_sms_contacts(family_id, broker)
    book.bump_and_push({target_uid}, broker, reason="sms_approve")

    # (e) the backlog, oldest first.
    delivered = undelivered = 0
    bid = externals_store.ensure_sms_backend(external)
    for row in held:
        pager_text = sms_twilio.pager_body(row.body)
        if not pager_text or sms_twilio.body_too_long(pager_text):
            held_sms_store.set_status([row.id], "too_long")
            continue
        result = routing.send(
            sender_uid=external.uid,
            recipient_alias=target.alias,
            kind="text",
            body=pager_text,
            origin_backend_kind="sms",
            origin_backend_id=bid,
            wire_id=row.id,
            ts=int(row.receivedAt.timestamp()),
        )
        if result.rejected:
            undelivered += 1
            continue
        # A deduplicated send (empty `messages`) was delivered earlier.
        held_sms_store.set_status([row.id], "delivered")
        delivered += 1
    return delivered, undelivered


def _approve_new_conversation(alert: Alert) -> None:
    if alert.subjectUid is None or alert.peerUid is None:
        raise HTTPException(status_code=400, detail="alert has no subject/peer to approve")
    allow_store.set_edge(alert.subjectUid, alert.peerUid, message=True, locate=False)
    allow_store.recompute_locatable_by_for_owner(alert.peerUid)


def _approve_contact_request(
    alert: Alert, req: ApproveAlertRequest, principal_uid: str, broker: BrokerClient
) -> None:
    """docs/CONTACT_REQ_DESIGN.md decision 2: approving a pager's contact
    request never creates a person. The target is resolved again now: a phone
    number always becomes the owner's family SMS contact (a person's sign-in
    phone is never a lookup key); an alias is an in-system user, and a link
    writes only the missing owner -> peer edge (never the peer's own edge to
    the owner)."""
    if req.mode == "create":
        raise HTTPException(status_code=400, detail="new people are added under Family > People")
    if alert.contactRequestKey is None:
        raise HTTPException(status_code=400, detail="alert has no linked contact request")
    request = contacts_store.get_request(alert.contactRequestKey)
    if request is None:
        raise HTTPException(status_code=404, detail="no such contact request")
    if request.status != "pending":
        raise HTTPException(status_code=409, detail="contact request already decided")
    owner = users_store.get_user(request.ownerUid)
    if owner is None:
        raise HTTPException(status_code=409, detail="the requesting user no longer exists")

    peer: User | None = None
    if request.phone is not None:
        peer = None
    elif request.alias is not None:
        peer = users_store.get_user_by_alias(request.alias)
    else:
        raise HTTPException(status_code=409, detail="contact request has no number or alias")

    if peer is None and request.phone is not None:
        if owner.familyId is None:
            raise HTTPException(status_code=409, detail="the requesting user has no family")
        try:
            contact = externals_store.get_or_create(
                owner.familyId, request.phone, req.name or request.name
            )
        except externals_store.ContactNameTaken as exc:
            raise _name_taken(owner.familyId, exc) from exc
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        _consent_by_admin(owner.familyId, contact.phone or request.phone, None)
        allow_store.set_edge(owner.uid, contact.uid, message=True, locate=False)
        rederive_family_sms_contacts(owner.familyId, broker)
    else:
        if (
            peer is None
            or peer.kind != "person"
            or peer.disabled
            or not _may_link(owner, peer)
        ):
            who = f"@{peer.alias}" if peer is not None else f"@{request.alias}"
            raise HTTPException(
                status_code=409, detail=f"{who} no longer has an edge to @{owner.alias}"
            )
        existing = allow_store.get_edge(owner.uid, peer.uid)
        if existing is None or not existing.message:
            allow_store.set_edge(
                owner.uid,
                peer.uid,
                message=True,
                locate=existing.locate if existing is not None else False,
            )

    contacts_store.approve(alert.contactRequestKey, decided_by=principal_uid)
    book.bump_and_push({owner.uid}, broker, reason="contact_approve")


def _may_link(owner: User, peer: User) -> bool:
    """Same family, or `peer` has a `message` edge to `owner`."""
    if book.same_family_persons(owner, peer):
        return True
    inbound = allow_store.get_edge(peer.uid, owner.uid)
    return inbound is not None and inbound.message


class HeldTextOut(BaseModel):
    id: str
    body: str
    receivedAt: datetime
    status: str


class HeldTextsOut(BaseModel):
    held: list[HeldTextOut]


@router.get("/alerts/{alert_id}/held")
def list_alert_held(alert_id: str, scope: FamilyScope) -> HeldTextsOut:
    """docs/RELAY_SMS_DESIGN.md decision 5: the texts behind an
    `sms_unknown` alert, oldest first, every status."""
    _, family_id = scope
    alert = alerts_store.get(family_id, alert_id)
    if alert is None:
        raise HTTPException(status_code=404, detail="no such alert")
    if alert.kind != "sms_unknown" or alert.peerPhone is None or alert.subjectUid is None:
        return HeldTextsOut(held=[])
    rows = held_sms_store.list_held(family_id, alert.peerPhone, alert.subjectUid, status=None)
    return HeldTextsOut(
        held=[
            HeldTextOut(id=r.id, body=r.body, receivedAt=r.receivedAt, status=r.status)
            for r in rows
        ]
    )


@router.post("/alerts/{alert_id}/approve", dependencies=[Depends(require_family_write_rate_limit)])
def approve_alert(
    alert_id: str,
    req: ApproveAlertRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    routing: Annotated[Routing, Depends(conversations_router.get_routing)],
) -> Alert | AlertDecision:
    principal, family_id = scope
    alert = _require_open_family_alert(alert_id, family_id)

    if alert.kind == "sms_unknown":
        delivered, undelivered = _approve_sms_unknown(alert, req, family_id, broker, routing)
        decided = alerts_store.decide(family_id, alert_id, "handled", principal.uid)
        return AlertDecision(alert=decided, delivered=delivered, undelivered=undelivered)
    elif alert.kind == "new_conversation":
        _approve_new_conversation(alert)
    elif alert.kind == "chat_unknown":
        raise HTTPException(
            status_code=400, detail="subscribe this conversation under People \u2192 Google Chat"
        )
    else:  # "contact_request"
        _approve_contact_request(alert, req, principal.uid, broker)

    return alerts_store.decide(family_id, alert_id, "handled", principal.uid)


def _reject_linked_contact_request(
    alert: Alert, reason: str, decided_by: str, broker: BrokerClient
) -> None:
    """A `contact_request` alert wraps a `contactRequests/{key}` doc
    (docs/FAMILIES_DESIGN.md §6 "Alert creation") -- blocking/dismissing the
    alert must also reject the underlying request, the same
    reject-then-bump-then-push sequence `POST
    /api/admin/contacts/{key}/reject` used to perform (`git show
    HEAD~3:relay/app/routers/admin.py`'s `reject_contact`), or the request
    stays `pending` forever: still counted against the pager's 5-pending cap
    and never removed from the device's book. A no-op if the request is
    already decided (e.g. a retried/duplicate alert action) -- matches this
    route's own idempotent-on-already-decided-alert behaviour."""
    if alert.kind != "contact_request" or alert.contactRequestKey is None:
        return
    request = contacts_store.get_request(alert.contactRequestKey)
    if request is None or request.status != "pending":
        return
    contacts_store.reject(alert.contactRequestKey, reason=reason, decided_by=decided_by)
    contacts_store.bump_book_version(request.deviceId)
    devcfg.push_book(request.deviceId, broker)


def _mark_held(family_id: str, alert: Alert, status: held_sms_store.HeldStatus) -> None:
    """Block / dismiss: the alert's still-`held` texts get `status`. Block
    covers the number's held texts for every member of the family."""
    if alert.peerPhone is None:
        return
    to_uid = None if status == "blocked" else alert.subjectUid
    if status != "blocked" and to_uid is None:
        return
    rows = held_sms_store.list_held(family_id, alert.peerPhone, to_uid, status="held")
    held_sms_store.set_status([r.id for r in rows], status)


@router.post("/alerts/{alert_id}/block", dependencies=[Depends(require_family_write_rate_limit)])
def block_alert(
    alert_id: str,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> Alert:
    """docs/FAMILIES_DESIGN.md §5.4: adds the alert's `peerPhone` to
    `families.blockedNumbers` (a no-op if the alert carries none -- e.g. a
    `new_conversation`/`contact_request` alert, which the web's Alert card
    never offers Block for, but nothing here assumes that)."""
    principal, family_id = scope
    alert = _require_open_family_alert(alert_id, family_id)
    if alert.kind == "chat_unknown":
        raise HTTPException(
            status_code=400, detail="use Ignore for a Google Chat conversation"
        )
    if alert.peerPhone is not None:
        families_store.add_blocked_number(family_id, alert.peerPhone)
        if alert.kind == "sms_unknown":
            _mark_held(family_id, alert, "blocked")
            # Their held rows are blocked too: decide every other open
            # alert for this number.
            for other in alerts_store.list_alerts(family_id, "open"):
                if (
                    other.id != alert.id
                    and other.kind == "sms_unknown"
                    and other.peerPhone == alert.peerPhone
                ):
                    alerts_store.decide(family_id, other.id, "handled", principal.uid)
    _reject_linked_contact_request(alert, "blocked", principal.uid, broker)
    return alerts_store.decide(family_id, alert_id, "handled", principal.uid)


@router.post("/alerts/{alert_id}/dismiss", dependencies=[Depends(require_family_write_rate_limit)])
def dismiss_alert(
    alert_id: str,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> Alert:
    principal, family_id = scope
    alert = _require_open_family_alert(alert_id, family_id)
    _reject_linked_contact_request(alert, "dismissed", principal.uid, broker)
    if alert.kind == "sms_unknown":
        _mark_held(family_id, alert, "dismissed")
    if alert.kind == "chat_unknown" and alert.bridgeId and alert.conversationId:
        # The conversation row stays `seen`, so a later text raises a fresh alert.
        held_chat_store.set_status(
            [
                r.id
                for r in held_chat_store.list_for_conversation(
                    bridge_conversations.row_id(alert.bridgeId, alert.conversationId)
                )
            ],
            "dismissed",
        )
    return alerts_store.decide(family_id, alert_id, "dismissed", principal.uid)

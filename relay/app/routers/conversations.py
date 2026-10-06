"""`/api/conversations/*` -- docs/SERVER_PLAN.md §5.1. Every message *write*
goes through here; the thread itself, contacts, delivery states, locations
and settings are read straight from Firestore by the web app (§5.1: "Reads
that the web app can do straight from Firestore ... have no API endpoint").
Both routes require a registered caller (`app.auth.require_user`).
"""

from __future__ import annotations

import time
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel

from app import book
from app import policy as policy_module
from app.auth import AuthedUser, principal_for, require_user
from app.broker import BrokerClient
from app.location import Location, NoLocatableDevice
from app.routing import Routing
from app.store import allow as allow_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.conversations import Conversation
from app.wire import (
    BODY_MAX_CODEPOINTS,
    BODY_MAX_UTF8_BYTES,
    strip_control_chars,
)

router = APIRouter(prefix="/api/conversations")


class SendMessageRequest(BaseModel):
    body: str


class SendMessageResponse(BaseModel):
    id: str


# docs/FAMILIES_DESIGN.md §2, §1 decision 7 / docs/FAMILIES_TASKS.md 3.1: the
# human text for a policy-gated 403's `{reason, message}` body. `policy_in`
# is formatted per-call (it names the recipient), so it isn't here.
_POLICY_REJECT_MESSAGES = {
    "policy_out": "Your family admin has limited who you can message.",
    "not_allowed": "You are not on each other's approved lists.",
}


def get_routing(request: Request) -> Routing:
    return request.app.state.routing


def get_location(request: Request) -> Location:
    return request.app.state.location


def get_broker(request: Request) -> BrokerClient:
    """Same `request.app.state.*` dependency shape as `app/routers/admin.py`'s
    own `get_broker` -- this router had no need for the broker before a group
    membership change started re-publishing the book (see
    `_push_book_to_members` below)."""
    return request.app.state.broker


def _resolve_message_alias(alias: str, sender_uid: str) -> str:
    """docs/FAMILIES_DESIGN.md §4 / docs/FAMILIES_TASKS.md 3.2: "an alias
    that parses as a phone number ... resolves to the external alias; the
    external is created on the fly only when the sender's outbound rule for
    externals is `any`, otherwise 404 `unknown_alias`."

    An `alias` that already resolves to a real user or an existing group is
    returned unchanged (covers a repeat send to an already-created
    external, whose alias *is* the phone's digits, without re-deriving
    anything). Only a value that resolves to nothing is tried as a phone
    number; if it isn't one, or the sender's outbound rule for externals
    (`app.policy.rule(sender.policy.out, "external")`) isn't `any`, `alias`
    is returned unchanged too -- `routing.send`'s own existing
    `unknown_alias` 404 fires exactly as it would for any other made-up
    alias, with no special case needed here for the rejection path."""
    if users_store.get_uid_for_alias(alias) is not None:
        return alias
    if conversations_store.get_by_alias(alias) is not None:
        return alias
    try:
        phone = externals_store.normalize_phone(alias)
    except ValueError:
        return alias
    sender = users_store.get_user(sender_uid)
    if sender is None or policy_module.rule(sender.policy.out, "external") != "any":
        return alias
    external = externals_store.get_or_create(phone, phone)
    return external.alias


@router.post("/{alias}/messages", status_code=201)
def send_message(
    alias: str,
    req: SendMessageRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    routing: Annotated[Routing, Depends(get_routing)],
) -> SendMessageResponse:
    # §3.1: strip control chars on ingest from the parent API, reject if
    # empty or oversize after stripping.
    body = strip_control_chars(req.body)
    if not body:
        raise HTTPException(
            status_code=400, detail="body is empty after stripping control characters"
        )
    if len(body) > BODY_MAX_CODEPOINTS:
        raise HTTPException(status_code=400, detail="body exceeds 160 Unicode code points")
    if len(body.encode("utf-8")) > BODY_MAX_UTF8_BYTES:
        raise HTTPException(status_code=400, detail="body exceeds 320 UTF-8 bytes")

    resolved_alias = _resolve_message_alias(alias, authed.uid)
    result = routing.send(
        sender_uid=authed.uid,
        recipient_alias=resolved_alias,
        kind="text",
        body=body,
        origin_backend_kind="webapp",
    )
    if result.rejected:
        reason = result.rejected[0].reason
        if reason == "unknown_alias":
            raise HTTPException(status_code=404, detail="unknown recipient")
        if reason == "not_member":
            raise HTTPException(status_code=403, detail="not allowed to message this recipient")
        # policy_out / policy_in / not_allowed -- docs/FAMILIES_TASKS.md 3.1:
        # the web reads `{reason, message}` (bare or under `detail`) to show
        # the server's own text rather than a generic one.
        message = (
            f"@{alias} is not accepting messages from you."
            if reason == "policy_in"
            else _POLICY_REJECT_MESSAGES[reason]
        )
        raise HTTPException(status_code=403, detail={"reason": reason, "message": message})
    if not result.messages:
        # Only reachable if `wireId` dedup fired, which never happens for an
        # API-originated send (no `wire_id` is passed) -- kept as a defensive
        # 500 rather than an assert so a future caller mistake is visible.
        raise HTTPException(status_code=500, detail="send failed")
    return SendMessageResponse(id=result.messages[0].id)


@router.post("/{alias}/messages/{msg_id}/read")
def mark_read(
    alias: str,
    msg_id: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
) -> dict[str, bool]:
    msg = messages_store.get_message(msg_id)
    if msg is None or msg.recipientUid != authed.uid:
        # Not found, or found but addressed to someone else -- same 404
        # either way so a probing client can't distinguish "doesn't exist"
        # from "exists but isn't yours".
        raise HTTPException(status_code=404, detail="no such message")

    # The `recipientUid` check above is the actual authz (correct and
    # sufficient on its own), but on its own it leaves the `{alias}` path
    # segment unchecked -- any alias would 200 regardless of whether it
    # named this message's conversation. Verify it does, 404ing (same
    # status/detail as "message not found") if not, so a mismatched alias
    # can't silently succeed. `alias` names either a user (the DM peer) or a
    # group (docs/GROUP_CHAT_DESIGN.md §3's "mark_read gains a group
    # branch") -- try the DM resolution first, since a group alias never has
    # a `uid` field to resolve (`app/store/conversations.py`'s docstring).
    peer_uid = users_store.get_uid_for_alias(alias)
    if peer_uid is not None:
        if msg.convKey != messages_store.conv_key(authed.uid, peer_uid):
            raise HTTPException(status_code=404, detail="no such message")
    else:
        group = conversations_store.get_by_alias(alias)
        if group is None or msg.convKey != group.convKey or authed.uid not in group.uids:
            raise HTTPException(status_code=404, detail="no such message")

    bid = messages_store.find_delivery_by_kind(msg, "webapp")
    if bid is None:
        raise HTTPException(status_code=404, detail="no webapp delivery for this message")

    # Clear this conversation's unread count for the reader in the same
    # transaction as the ack -- see `messages_store.apply_delivery_ack`'s
    # docstring. This is the only thing that clears `unread`;
    # `create_message` only ever increments it.
    messages_store.apply_delivery_ack(
        msg.id, bid, None, "read", int(time.time()), clear_unread_uid=authed.uid
    )
    return {"ok": True}


# ---------------------------------------------------------------------------
# Group admin API -- docs/GROUP_CHAT_DESIGN.md §3, docs/FAMILIES_DESIGN.md §4.
# Creation moved to `POST /api/family/groups` (`app/routers/family.py`,
# docs/FAMILIES_TASKS.md 1.3) -- `_push_book_to_members`/
# `_create_missing_message_edges` below are reused from there, hence not
# module-private in practice even though the leading underscore (an internal
# helper of this router, not a public API) is unchanged. Join
# (`add_group_member` below) and leave stay mounted here under
# `/api/conversations`, `require_user`.
# ---------------------------------------------------------------------------


def _push_book_to_members(uids: list[str], broker: BrokerClient) -> None:
    """A group create/join/leave changes the members' derived books
    (`build/bench-logs/group-acceptance.md`'s "Gap found"); bump and push
    each affected owner's devices -- `app/book.py`'s `bump_and_push`
    (docs/ADDRESS_BOOK_DESIGN.md decision 6), which de-duplicates by
    device. A uid with no device is a no-op."""
    book.bump_and_push(uids, broker, reason="group")


def _create_missing_message_edges(member_uids: list[str]) -> None:
    """Decision 2 (docs/GROUP_CHAT_DESIGN.md, "Decisions" section): creating
    or joining a group auto-creates `allow` edges in both directions between
    every member pair, so the send-time allow-list gate never
    partial-delivers. Only *missing* edges are created -- an edge that
    already exists (whatever its `message`/`locate` flags) is left alone, so
    this can never silently widen or narrow an admin's existing, deliberate
    allow-list decision.

    Message-only, never `locate` -- both group *create*
    (`app/routers/family.py`'s `create_group`) and group *join*
    (`add_group_member` below) call this same function now
    (docs/FAMILIES_DESIGN.md §1 decision 9 for join;
    docs/FAMILIES_TASKS.md 3.2 addition (b) extends the same "no silent
    mutual-`locate` side effect" rule to create, superseding this module's
    earlier `_create_missing_allow_edges`, which passed `locate=True` and
    has been removed -- a member pair spanning two families would otherwise
    have its first group together silently fail `allow_store.
    check_locate_family` the moment that check is enforced anywhere on this
    path)."""
    for a in member_uids:
        for b in member_uids:
            if a == b:
                continue
            if allow_store.get_edge(a, b) is None:
                allow_store.set_edge(a, b, message=True, locate=False)


class AddMemberRequest(BaseModel):
    uid: str


def _group_creator_family_id(group: Conversation) -> str | None:
    creator = users_store.get_user(group.createdBy) if group.createdBy else None
    return creator.familyId if creator is not None else None


@router.post("/{alias}/members", status_code=200)
def add_group_member(
    alias: str,
    req: AddMemberRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> Conversation:
    group = conversations_store.get_by_alias(alias)
    if group is None:
        raise HTTPException(status_code=404, detail="no such group")

    # docs/FAMILIES_TASKS.md 1.3 / docs/FAMILIES_DESIGN.md §4: joining is no
    # longer self-service for any current member -- the caller must be a
    # family admin of the group *creator's* family (or super).
    principal = principal_for(authed)
    is_authorized = principal.role == "super" or (
        principal.role == "admin"
        and principal.family_id is not None
        and principal.family_id == _group_creator_family_id(group)
    )
    if not is_authorized:
        raise HTTPException(
            status_code=403,
            detail="only a family admin of the group creator's family may add members",
        )
    if users_store.get_user(req.uid) is None:
        raise HTTPException(status_code=404, detail=f"no such user: {req.uid!r}")

    updated = conversations_store.add_member(group.convKey, req.uid)
    # docs/FAMILIES_DESIGN.md §1 decision 9: `message` edges only, never
    # `locate` -- see `_create_missing_message_edges`'s docstring.
    _create_missing_message_edges(updated.uids)
    # The new member's book gains this group; see `_push_book_to_members`'s
    # docstring for why pushing to every current member (not just the new
    # one) is harmless.
    _push_book_to_members(updated.uids, broker)
    return updated


@router.delete("/{alias}/members/me", status_code=200)
def leave_group(
    alias: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> Conversation:
    group = conversations_store.get_by_alias(alias)
    if group is None:
        raise HTTPException(status_code=404, detail="no such group")
    if authed.uid not in group.uids:
        raise HTTPException(status_code=404, detail="not a member of this group")
    updated = conversations_store.remove_member(group.convKey, authed.uid)
    # The leaver's own book loses this group -- push to the leaver
    # specifically (they're no longer in `updated.uids`, so
    # `_push_book_to_members` would otherwise miss them entirely).
    _push_book_to_members([authed.uid], broker)
    return updated


# ---------------------------------------------------------------------------
# /locate -- docs/SERVER_PLAN.md §5.1, §5.6; docs/PROTOCOL.md §13.3 rules 5-7
# ---------------------------------------------------------------------------


class LocateFixOut(BaseModel):
    lat: float
    lon: float
    accM: int | None = None
    fixTs: int
    src: str = "gnss"


class LocateResponse(BaseModel):
    # `requestId` is the in-flight (fresh or coalesced) `loc_req` message id
    # -- None on the `cached` path, where no `loc_req` was ever created
    # (docs/SERVER_PLAN.md §5.6: "answer directly from that cached fix ...
    # no wire message, no locReqs doc created"). This is an intentional
    # extension of §5.1's API sketch ("202 {request_id}"), which predates
    # §5.6's cached-answer detail.
    requestId: str | None = None
    cached: bool = False
    fix: LocateFixOut | None = None


@router.post("/{alias}/locate", status_code=202)
def locate(
    alias: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
    location: Annotated[Location, Depends(get_location)],
) -> LocateResponse:
    target_uid = users_store.get_uid_for_alias(alias)
    if target_uid is None:
        raise HTTPException(status_code=404, detail="unknown recipient")

    # Owner decision (27 Sep 2026): a user may always locate their *own*
    # device, no `locate` edge needed. Only the edge check is skipped --
    # `Location.locate`'s §13.3 rules 5-7 (one in-flight loc_req, 60 s
    # cache) apply unchanged. docs/FAMILIES_TASKS.md 2.3 adds one more
    # bypass: a family admin (role `admin`, claims-only) of the target's
    # `familyId` may also locate without a `locate` edge -- `target_uid`
    # doubles as the device owner in this single-device-per-user model, so
    # there's no separate "target's owner" case to handle here. Super gets
    # no bypass at all: it must hold a `locate` edge like anyone else
    # (docs/FAMILIES_DESIGN.md §1 decision 5: "Super may read stored fixes
    # everywhere but does not get `/locate` by role").
    if target_uid != authed.uid:
        principal = principal_for(authed)
        target_user = users_store.get_user(target_uid)
        is_family_admin = (
            principal.role == "admin"
            and principal.family_id is not None
            and target_user is not None
            and target_user.familyId == principal.family_id
        )
        if not is_family_admin:
            edge = allow_store.get_edge(authed.uid, target_uid)
            if edge is None or not edge.locate:
                raise HTTPException(status_code=403, detail="not allowed to locate this user")

    # docs/SERVER_PLAN.md models one pager device per user as the common
    # case. For the (in principle possible) multi-device case: pick the
    # lowest device id (lexicographic) among the target's non-revoked
    # devices -- deterministic but otherwise arbitrary. Supporting it
    # properly would need a way for the caller to name *which* device a
    # `/locate` call means.
    candidates = [
        d for d in devices_store.list_devices(owner_uid=target_uid) if d.revokedAt is None
    ]
    if not candidates:
        raise HTTPException(status_code=409, detail="no locatable device")
    device = min(candidates, key=lambda d: d.id)

    try:
        outcome = location.locate(requester_uid=authed.uid, device=device)
    except NoLocatableDevice:
        raise HTTPException(status_code=409, detail="no locatable device") from None

    fix_out = None
    if outcome.fix is not None:
        fix_out = LocateFixOut(
            lat=outcome.fix.lat,
            lon=outcome.fix.lon,
            accM=outcome.fix.accM,
            fixTs=outcome.fix.fixTs,
            src=outcome.fix.src,
        )
    return LocateResponse(requestId=outcome.request_id, cached=outcome.cached, fix=fix_out)

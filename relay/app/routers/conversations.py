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
from google.api_core.exceptions import AlreadyExists
from pydantic import BaseModel, Field

from app.auth import AuthedUser, require_admin, require_user
from app.location import Location, NoLocatableDevice
from app.routing import Routing
from app.store import allow as allow_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.conversations import Conversation
from app.store.users import InvalidAlias
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


def get_routing(request: Request) -> Routing:
    return request.app.state.routing


def get_location(request: Request) -> Location:
    return request.app.state.location


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

    result = routing.send(
        sender_uid=authed.uid,
        recipient_alias=alias,
        kind="text",
        body=body,
        origin_backend_kind="webapp",
    )
    if result.rejected:
        reason = result.rejected[0].reason
        if reason == "unknown_alias":
            raise HTTPException(status_code=404, detail="unknown recipient")
        raise HTTPException(status_code=403, detail="not allowed to message this recipient")
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
# Group admin API -- docs/GROUP_CHAT_DESIGN.md §3. All `require_user`;
# creation is admin-only in v1 (`require_admin`).
# ---------------------------------------------------------------------------


class CreateGroupRequest(BaseModel):
    name: str
    alias: str
    memberUids: list[str] = Field(min_length=2)


class CreateGroupResponse(BaseModel):
    convKey: str
    alias: str


def _create_missing_allow_edges(member_uids: list[str]) -> None:
    """Decision 2 (docs/GROUP_CHAT_DESIGN.md, "Decisions" section): creating
    or joining a group auto-creates `allow` edges in both directions between
    every member pair, so the send-time allow-list gate never
    partial-delivers. Only *missing* edges are created -- an edge that
    already exists (whatever its `message`/`locate` flags) is left alone, so
    this can never silently widen or narrow an admin's existing, deliberate
    allow-list decision."""
    for a in member_uids:
        for b in member_uids:
            if a == b:
                continue
            if allow_store.get_edge(a, b) is None:
                allow_store.set_edge(a, b, message=True, locate=True)


@router.post("", status_code=201)
def create_group(
    req: CreateGroupRequest,
    authed: Annotated[AuthedUser, Depends(require_admin)],
) -> CreateGroupResponse:
    member_uids = sorted(set(req.memberUids))
    for uid in member_uids:
        if users_store.get_user(uid) is None:
            raise HTTPException(status_code=404, detail=f"no such user: {uid!r}")

    try:
        conv = conversations_store.create_group(
            name=req.name, alias=req.alias, member_uids=member_uids, created_by=authed.uid
        )
    except InvalidAlias as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except AlreadyExists as exc:
        raise HTTPException(status_code=409, detail="alias already taken") from exc

    _create_missing_allow_edges(conv.uids)
    return CreateGroupResponse(convKey=conv.convKey, alias=req.alias)


class AddMemberRequest(BaseModel):
    uid: str


@router.post("/{alias}/members", status_code=200)
def add_group_member(
    alias: str,
    req: AddMemberRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
) -> Conversation:
    group = conversations_store.get_by_alias(alias)
    if group is None:
        raise HTTPException(status_code=404, detail="no such group")
    if authed.uid not in group.uids:
        raise HTTPException(status_code=403, detail="not a member of this group")
    if users_store.get_user(req.uid) is None:
        raise HTTPException(status_code=404, detail=f"no such user: {req.uid!r}")

    updated = conversations_store.add_member(group.convKey, req.uid)
    # Decision 2 again -- joining auto-creates the missing edges between the
    # new member and every *existing* member (the new member's own pairing
    # with `authed.uid` is covered by this too, since `authed.uid` is one of
    # `updated.uids`).
    _create_missing_allow_edges(updated.uids)
    return updated


@router.delete("/{alias}/members/me", status_code=200)
def leave_group(
    alias: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
) -> Conversation:
    group = conversations_store.get_by_alias(alias)
    if group is None:
        raise HTTPException(status_code=404, detail="no such group")
    if authed.uid not in group.uids:
        raise HTTPException(status_code=404, detail="not a member of this group")
    return conversations_store.remove_member(group.convKey, authed.uid)


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

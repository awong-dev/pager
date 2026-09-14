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

from app.auth import AuthedUser, require_user
from app.routing import Routing
from app.store import messages as messages_store
from app.store import users as users_store
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


@router.post("/{alias}/messages", status_code=201)
def send_message(
    alias: str,
    req: SendMessageRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    routing: Annotated[Routing, Depends(get_routing)],
) -> SendMessageResponse:
    # §3.1: strip control chars on ingest from the parent API, reject if
    # empty or oversize after stripping -- same rule `routers/legacy.py`
    # applies to the MVP send endpoint.
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

    # S3b `build finding`: the `recipientUid` check above is the actual
    # authz (correct and sufficient on its own), but until this check the
    # `{alias}` path segment was otherwise ignored -- any alias 200'd
    # regardless of whether it named this message's conversation at all.
    # Verify it does, 404ing (same status/detail as "message not found") if
    # not, so a mismatched alias can't silently succeed.
    peer_uid = users_store.get_uid_for_alias(alias)
    if peer_uid is None or msg.convKey != messages_store.conv_key(authed.uid, peer_uid):
        raise HTTPException(status_code=404, detail="no such message")

    bid = messages_store.find_delivery_by_kind(msg, "webapp")
    if bid is None:
        raise HTTPException(status_code=404, detail="no webapp delivery for this message")

    # S3a `build finding`: clear this conversation's unread count for the
    # reader in the same transaction as the ack -- see
    # `messages_store.apply_delivery_ack`'s docstring; previously `unread`
    # only ever incremented (`create_message`) and nothing ever cleared it.
    messages_store.apply_delivery_ack(
        msg.id, bid, None, "read", int(time.time()), clear_unread_uid=authed.uid
    )
    return {"ok": True}

"""`/api/family/bridges*` and `/api/family/members/{uid}/chat` --
docs/BRIDGE_PHONE_DESIGN.md decisions 1-2, 7, 10, 12 (O8: one file for the
bridge-phone admin API and the Google Chat subscribe API).

Every route is a family admin (or super naming a family) acting on their own
family's bridges; a bridge of another family is a 404.
"""

from __future__ import annotations

import logging
from datetime import datetime
from typing import Annotated, Any

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel, ConfigDict, Field

from app import bridge_numbers, chat_subscribe, devcfg
from app.broker import BrokerClient
from app.chat_subscribe import ChatError
from app.routers.bridge import normalize_optional_phone
from app.routers.family import (
    FamilyScope,
    _require_family_member,
    require_family_write_rate_limit,
)
from app.routing import Routing
from app.store import bridge_conversations, bridge_outbox
from app.store import bridges as bridges_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.bridge_conversations import BridgeConversation

logger = logging.getLogger("relay.family_bridges")

router = APIRouter(prefix="/api/family")


def get_broker(request: Request) -> BrokerClient:
    return request.app.state.broker


def get_routing(request: Request) -> Routing:
    return request.app.state.routing


def _chat_error(exc: ChatError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.detail)


def _require_bridge(bridge_id: str, family_id: str) -> bridges_store.Bridge:
    bridge = bridges_store.get(bridge_id)
    if bridge is None or bridge.familyId != family_id:
        raise HTTPException(status_code=404, detail="no such bridge")
    return bridge


# ---------------------------------------------------------------------------
# Google Chat conversations (decisions 7, 10)
# ---------------------------------------------------------------------------


class RosterEntry(BaseModel):
    model_config = ConfigDict(extra="ignore")

    name: str = Field(min_length=1, max_length=200)
    nick: str = Field(max_length=64)


class SubscribeRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    pagerName: str = Field(max_length=200)
    canReply: bool = True
    roster: list[RosterEntry] = Field(default_factory=list, max_length=64)


class PatchConversationRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    pagerName: str | None = Field(default=None, max_length=200)
    canReply: bool | None = None
    paused: bool | None = None
    roster: list[RosterEntry] | None = Field(default=None, max_length=64)


class ConversationOut(BaseModel):
    ref: str
    bridgeId: str
    conversationId: str
    source: str
    title: str | None
    isGroup: bool
    link: str | None
    people: list[str]
    lastPreview: str
    lastAt: datetime | None
    firstSeenAt: datetime | None
    inspectedAt: datetime | None
    heldCount: int
    status: str
    uid: str | None
    convKey: str | None
    pagerName: str | None
    customName: bool
    alertId: str | None
    # Subscribed rows only: the alias the pager addresses, whether the kid can
    # reply, the roster (group) and whether it is within the pager's first 32.
    alias: str | None = None
    canReply: bool | None = None
    roster: list[RosterEntry] | None = None
    onPager: bool | None = None


class ChatTabOut(BaseModel):
    subscribed: list[ConversationOut]
    seen: list[ConversationOut]
    bridges: list[dict[str, Any]]


class SubscribeOut(BaseModel):
    uid: str
    convKey: str | None
    alias: str
    delivered: int
    undelivered: int
    conversation: ConversationOut


def _row_out(row: BridgeConversation, *, on_pager: set[str] | None = None) -> ConversationOut:
    out = ConversationOut(
        ref=row.ref,
        bridgeId=row.bridgeId,
        conversationId=row.conversationId,
        source=row.source,
        title=row.title,
        isGroup=row.isGroup,
        link=row.link,
        people=row.people,
        lastPreview=row.lastPreview,
        lastAt=row.lastAt,
        firstSeenAt=row.firstSeenAt,
        inspectedAt=row.inspectedAt,
        heldCount=row.heldCount,
        status=row.status,
        uid=row.uid,
        convKey=row.convKey,
        pagerName=row.pagerName,
        customName=row.customName,
        alertId=row.alertId,
    )
    if row.status in ("subscribed", "paused") and row.uid:
        ext = users_store.get_user(row.uid)
        out.alias = ext.alias if ext is not None else None
        out.canReply = (ext.chat or {}).get("canReply") is not False if ext is not None else None
        if row.isGroup and row.convKey:
            group = conversations_store.get_bridge_group(row.convKey)
            if group is not None:
                out.alias = group.alias
                out.roster = [RosterEntry(name=n, nick=k) for k, n in sorted(group.roster.items())]
        if on_pager is not None:
            out.onPager = out.alias in on_pager
    return out


def _on_pager_aliases(owner_uid: str) -> set[str]:
    devices = sorted(devices_store.list_devices(owner_uid=owner_uid), key=lambda d: d.id)
    default_alias = devcfg._default_alias(devices[0]) if devices else None
    ordered = devcfg._ordered_contacts(owner_uid, default_alias)
    return {c["a"] for c in ordered[: devcfg.MAX_PULL_CONTACTS]}


@router.get("/members/{uid}/chat")
def member_chat(uid: str, scope: FamilyScope) -> ChatTabOut:
    """The Google Chat tab for member `uid`: subscribed (and paused)
    conversations with `onPager`, and the `seen` / `ignored` ones."""
    _, family_id = scope
    _require_family_member(uid, family_id)
    rows = bridge_conversations.list_for_owner(uid)
    on_pager = _on_pager_aliases(uid)
    return ChatTabOut(
        subscribed=[
            _row_out(r, on_pager=on_pager) for r in rows if r.status in ("subscribed", "paused")
        ],
        seen=[_row_out(r) for r in rows if r.status in ("seen", "ignored")],
        bridges=[
            {"id": b.id, "label": b.label, "paired": b.paired, "caps": b.caps.model_dump()}
            for b in bridges_store.list_for_owner(uid)
            if b.familyId == family_id
        ],
    )


def _row_or_404(bridge: bridges_store.Bridge, ref: str) -> BridgeConversation:
    row = bridge_conversations.get_by_ref(bridge.id, ref)
    if row is None:
        raise HTTPException(status_code=404, detail="no such conversation")
    return row


@router.post(
    "/bridges/{bridge_id}/conversations/{ref}/subscribe",
    dependencies=[Depends(require_family_write_rate_limit)],
)
def subscribe_conversation(
    bridge_id: str,
    ref: str,
    req: SubscribeRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
    routing: Annotated[Routing, Depends(get_routing)],
) -> SubscribeOut:
    principal, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    row = _row_or_404(bridge, ref)
    try:
        result = chat_subscribe.subscribe(
            family_id,
            bridge,
            row.conversationId,
            req.pagerName,
            req.canReply,
            [(r.name, r.nick) for r in req.roster],
            principal.uid,
            broker,
            routing,
        )
    except ChatError as exc:
        raise _chat_error(exc) from exc
    fresh = bridge_conversations.get_by_ref(bridge.id, ref)
    assert fresh is not None
    return SubscribeOut(
        uid=result.uid,
        convKey=result.convKey,
        alias=result.alias,
        delivered=result.delivered,
        undelivered=result.undelivered,
        conversation=_row_out(fresh, on_pager=_on_pager_aliases(bridge.ownerUid)),
    )


@router.post(
    "/bridges/{bridge_id}/conversations/{ref}/ignore",
    dependencies=[Depends(require_family_write_rate_limit)],
)
def ignore_conversation(bridge_id: str, ref: str, scope: FamilyScope) -> ConversationOut:
    principal, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    row = _row_or_404(bridge, ref)
    try:
        fresh = chat_subscribe.ignore(family_id, bridge, row.conversationId, principal.uid)
    except ChatError as exc:
        raise _chat_error(exc) from exc
    return _row_out(fresh)


@router.patch(
    "/bridges/{bridge_id}/conversations/{ref}",
    dependencies=[Depends(require_family_write_rate_limit)],
)
def patch_conversation(
    bridge_id: str,
    ref: str,
    req: PatchConversationRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> ConversationOut:
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    row = _row_or_404(bridge, ref)
    try:
        fresh = chat_subscribe.patch(
            family_id,
            bridge,
            row.conversationId,
            pager_name=req.pagerName,
            can_reply=req.canReply,
            paused=req.paused,
            roster=None if req.roster is None else [(r.name, r.nick) for r in req.roster],
            broker=broker,
        )
    except ChatError as exc:
        raise _chat_error(exc) from exc
    return _row_out(fresh, on_pager=_on_pager_aliases(bridge.ownerUid))


@router.delete(
    "/bridges/{bridge_id}/conversations/{ref}",
    dependencies=[Depends(require_family_write_rate_limit)],
)
def unsubscribe_conversation(
    bridge_id: str,
    ref: str,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> ConversationOut:
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    row = _row_or_404(bridge, ref)
    try:
        fresh = chat_subscribe.unsubscribe(family_id, bridge, row.conversationId, broker)
    except ChatError as exc:
        raise _chat_error(exc) from exc
    return _row_out(fresh)


GOOGLE_LINK_PREFIXES = (
    "https://chat.google.com/",
    "https://mail.google.com/chat/",
    "https://voice.google.com/",
)


class InspectRequest(BaseModel):
    link: str = Field(max_length=2048)


@router.post(
    "/bridges/{bridge_id}/inspect",
    status_code=202,
    dependencies=[Depends(require_family_write_rate_limit)],
)
def inspect_link(bridge_id: str, req: InspectRequest, scope: FamilyScope) -> dict[str, str]:
    """Decision 10: ask the phone to open a pasted Chat / Voice link and
    report the conversation. 202 `{outboxId}`."""
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    link = req.link.strip()
    if not link.startswith(GOOGLE_LINK_PREFIXES):
        raise HTTPException(status_code=422, detail="not a Google Chat or Google Voice link")
    if not bridge.paired:
        raise HTTPException(status_code=409, detail="the bridge phone is not paired")
    item = bridge_outbox.enqueue_inspect(bridge, link)
    return {"outboxId": item.id}


# ---------------------------------------------------------------------------
# Bridge phones: list / create / code / reassign / numbers / accept-sim / unpair
# (decisions 1-3, O1 revised)
# ---------------------------------------------------------------------------


class BridgeOut(BaseModel):
    """A bridge as the web sees it: never `tokenHash` or `fcmToken`."""

    id: str
    ownerUid: str
    ownerAlias: str | None
    ownerName: str | None
    label: str
    paired: bool
    pairedAt: datetime | None
    lastSeenAt: datetime | None
    simNumber: str | None
    voiceNumber: str | None
    status: bridges_store.BridgeStatus
    caps: bridges_store.BridgeCaps
    createdAt: datetime | None
    # Only on `PATCH` (reassign): how many subscribed Google Chat
    # conversations were unsubscribed because the owner changed.
    unsubscribed: int = 0


class CreateBridgeRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    ownerUid: str
    label: str = Field(min_length=1, max_length=80)


class PairCodeOut(BaseModel):
    code: str
    expiresAt: datetime


class CreateBridgeOut(PairCodeOut):
    bridge: BridgeOut


class PatchBridgeRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    ownerUid: str | None = None
    label: str | None = Field(default=None, min_length=1, max_length=80)
    simNumber: str | None = Field(default=None, max_length=32)
    voiceNumber: str | None = Field(default=None, max_length=32)


def _bridge_out(bridge: bridges_store.Bridge) -> BridgeOut:
    owner = users_store.get_user(bridge.ownerUid)
    return BridgeOut(
        id=bridge.id,
        ownerUid=bridge.ownerUid,
        ownerAlias=owner.alias if owner is not None else None,
        ownerName=owner.displayName if owner is not None else None,
        label=bridge.label,
        paired=bridge.paired,
        pairedAt=bridge.pairedAt,
        lastSeenAt=bridge.lastSeenAt,
        simNumber=bridge.simNumber,
        voiceNumber=bridge.voiceNumber,
        status=bridge.status,
        caps=bridge.caps,
        createdAt=bridge.createdAt,
    )


def _require_active_person(uid: str, family_id: str) -> users_store.User:
    owner = users_store.get_user(uid)
    if owner is None or owner.kind != "person" or owner.familyId != family_id:
        raise HTTPException(status_code=404, detail="no such member in this family")
    if owner.disabled:
        raise HTTPException(status_code=409, detail="that member is disabled")
    return owner


@router.get("/bridges")
def list_bridges(scope: FamilyScope) -> list[BridgeOut]:
    _, family_id = scope
    return [_bridge_out(b) for b in bridges_store.list_for_family(family_id)]


@router.post(
    "/bridges", status_code=201, dependencies=[Depends(require_family_write_rate_limit)]
)
def create_bridge(req: CreateBridgeRequest, scope: FamilyScope) -> CreateBridgeOut:
    """Decision 2: creates the (unpaired) row and a 10-minute pairing code."""
    principal, family_id = scope
    _require_active_person(req.ownerUid, family_id)
    bridge = bridges_store.create(req.ownerUid, family_id, req.label.strip(), principal.uid)
    code, expires_at = bridges_store.create_pair_code(bridge.id)
    return CreateBridgeOut(bridge=_bridge_out(bridge), code=code, expiresAt=expires_at)


@router.post(
    "/bridges/{bridge_id}/code", dependencies=[Depends(require_family_write_rate_limit)]
)
def new_pair_code(bridge_id: str, scope: FamilyScope) -> PairCodeOut:
    """A new code, only while the bridge has no token (never paired, or
    unpaired)."""
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    if bridge.paired:
        raise HTTPException(status_code=409, detail="this bridge phone is already paired")
    code, expires_at = bridges_store.create_pair_code(bridge.id)
    return PairCodeOut(code=code, expiresAt=expires_at)


def _normalize(raw: str | None, field: str) -> str | None:
    return normalize_optional_phone(raw, field)


def _number_taken(owner_uid: str, number: str, bridge: bridges_store.Bridge) -> None:
    """409 when `number` already belongs to another member or another bridge
    (the bridge's own previous owner holding it through this very bridge does
    not count: reassigning releases it)."""
    bridge_id = bridge.id
    holder = users_store.get_uid_for_sms_number(number)
    own_release = holder == bridge.ownerUid and number in (bridge.simNumber, bridge.voiceNumber)
    if holder is not None and holder != owner_uid and not own_release:
        holder_user = users_store.get_user(holder)
        if holder_user is not None and holder_user.smsNumber == number:
            raise HTTPException(
                status_code=409, detail=f"that number belongs to @{holder_user.alias}"
            )
    other = bridges_store.get_by_sms_number(number)
    if other is not None and other.id != bridge_id:
        raise HTTPException(status_code=409, detail="that number is used by another bridge phone")


@router.patch("/bridges/{bridge_id}", dependencies=[Depends(require_family_write_rate_limit)])
def patch_bridge(
    bridge_id: str,
    req: PatchBridgeRequest,
    scope: FamilyScope,
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> BridgeOut:
    """Label, reassign (`ownerUid`) and the accepted numbers (`simNumber`,
    `voiceNumber`: a non-null string sets, `""` clears, absent leaves).
    The owner's `smsNumber` follows the bridge: SIM if present, else Voice
    (O1 revised); a number held by another member or bridge is a 409."""
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    fields = req.model_fields_set

    new_sim = (
        _normalize(req.simNumber, "simNumber") if "simNumber" in fields else bridge.simNumber
    )
    new_voice = (
        _normalize(req.voiceNumber, "voiceNumber") if "voiceNumber" in fields else bridge.voiceNumber
    )
    new_owner_uid = req.ownerUid if req.ownerUid is not None else bridge.ownerUid
    new_owner = _require_active_person(new_owner_uid, family_id)
    owner_changed = new_owner_uid != bridge.ownerUid
    numbers_changed = (new_sim, new_voice) != (bridge.simNumber, bridge.voiceNumber)
    if not owner_changed and not numbers_changed:
        if req.label is not None:
            bridges_store.set_label(bridge.id, req.label.strip())
        fresh = bridges_store.get(bridge.id)
        assert fresh is not None
        return _bridge_out(fresh)

    for number in (new_sim, new_voice):
        if number is not None:
            _number_taken(new_owner_uid, number, bridge)

    if req.label is not None:
        bridges_store.set_label(bridge.id, req.label.strip())
    unsubscribed = 0
    if owner_changed:
        # The old owner's subscriptions, queued texts and last-used channels
        # go with them; this runs while `bridge.ownerUid` is still the old one.
        principal, _ = scope
        unsubscribed = chat_subscribe.reassign_conversations(
            family_id, bridge, new_owner_uid, principal.uid, broker
        )
        for item in bridge_outbox.fail_pending(bridge.id, "reassigned"):
            if item.msgId and item.bid:
                messages_store.mark_delivery_failed_if_queued(
                    item.msgId, item.bid, error="reassigned"
                )
    if bridge.paired:
        # The previous owner (or the previous numbers) are released first.
        bridge_numbers.release_numbers(bridge, broker)
    caps = bridge_numbers.caps_for(
        bridge.status.smsCapable or bridge.caps.sms, bridge.caps.gchat, new_sim, new_voice
    )
    if owner_changed:
        bridges_store.set_owner(bridge.id, new_owner_uid)
    bridges_store.set_numbers(bridge.id, sim_number=new_sim, voice_number=new_voice, caps=caps)
    bridges_store.set_error(bridge.id, None)
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    if fresh.paired:
        bridge_numbers.apply_numbers(fresh, broker)
    if owner_changed:
        bridge_numbers.rederive(new_owner.uid, broker)
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    out = _bridge_out(fresh)
    out.unsubscribed = unsubscribed
    return out


@router.post(
    "/bridges/{bridge_id}/accept-sim", dependencies=[Depends(require_family_write_rate_limit)]
)
def accept_sim(
    bridge_id: str, scope: FamilyScope, broker: Annotated[BrokerClient, Depends(get_broker)]
) -> BridgeOut:
    """Decision 3: the SIM number the phone reports becomes the accepted one
    (same `smsNumber` handling as pairing), clearing `status.error`."""
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    reported = bridge.status.simNumber
    if not bridge.paired:
        raise HTTPException(status_code=409, detail="the bridge phone is not paired")
    if reported is None:
        raise HTTPException(status_code=409, detail="the phone has not reported a SIM number")
    _require_active_person(bridge.ownerUid, family_id)
    if reported != bridge.simNumber:
        _number_taken(bridge.ownerUid, reported, bridge)
        bridge_numbers.release_numbers(bridge, broker)
        caps = bridge_numbers.caps_for(
            bridge.status.smsCapable or bridge.caps.sms,
            bridge.caps.gchat,
            reported,
            bridge.voiceNumber,
        )
        bridges_store.set_numbers(
            bridge.id, sim_number=reported, voice_number=bridge.voiceNumber, caps=caps
        )
    bridges_store.set_error(bridge.id, None)
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    bridge_numbers.apply_numbers(fresh, broker)
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    return _bridge_out(fresh)


@router.delete("/bridges/{bridge_id}", dependencies=[Depends(require_family_write_rate_limit)])
def unpair_bridge(
    bridge_id: str, scope: FamilyScope, broker: Annotated[BrokerClient, Depends(get_broker)]
) -> BridgeOut:
    """Decision 2: clears the token and FCM token, clears the owner's
    `smsNumber` if it is one of the bridge's numbers, fails every pending
    outbox row `unpaired` (and its delivery). The row, every contact and
    every conversation stay."""
    _, family_id = scope
    bridge = _require_bridge(bridge_id, family_id)
    bridges_store.unpair(bridge.id)
    for item in bridge_outbox.fail_pending(bridge.id, "unpaired"):
        if item.msgId and item.bid:
            messages_store.mark_delivery_failed_if_queued(item.msgId, item.bid, error="unpaired")
    bridge_numbers.release_numbers(bridge, broker)
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    logger.info("bridge unpaired bridge=%s owner=%s", bridge.id, bridge.ownerUid)
    return _bridge_out(fresh)

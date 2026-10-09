"""`/bridge/*` -- the headless bridge phone's API (docs/BRIDGE_PHONE_DESIGN.md).

`POST /bridge/pair` is unauthenticated (the pairing code is the credential);
every other route authenticates `Authorization: Bearer <bridgeId>.<secret>`
through `app.bridgeauth.require_bridge`.
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
import re
import time
from datetime import UTC, datetime, timedelta
from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response
from pydantic import BaseModel, ConfigDict, Field, field_validator
from starlette.concurrency import run_in_threadpool

from app import alerts as alerts_module
from app import bridge_numbers, chat_subscribe
from app.backends.sms_twilio import voice_link
from app.bridgeauth import mint_token, require_bridge
from app.inbound_text import handle_text, placeholder_for
from app.notify import sms as sms_client
from app.routers.webhooks import _check_webhook_ip_rate_limit
from app.store import bridge_conversations, bridge_outbox
from app.store import bridges as bridges_store
from app.store import externals as externals_store
from app.store import held_chat as held_chat_store
from app.store import messages as messages_store
from app.store import rate_limits as rate_limits_store
from app.store import users as users_store

logger = logging.getLogger("relay.bridge")

router = APIRouter(prefix="/bridge")

PAIR_GLOBAL_LIMIT = 10
PAIR_GLOBAL_WINDOW_S = 60


class PairCaps(BaseModel):
    model_config = ConfigDict(extra="ignore")

    sms: bool = False
    gchat: bool = False
    gvoice: bool = False


class PairRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    code: str = Field(max_length=32)
    version: str | None = Field(default=None, max_length=64)
    accounts: list[Annotated[str, Field(max_length=200)]] = Field(default_factory=list, max_length=10)
    simNumber: str | None = Field(default=None, max_length=32)
    voiceNumber: str | None = Field(default=None, max_length=32)
    caps: PairCaps = Field(default_factory=PairCaps)


class PairResponse(BaseModel):
    bridgeId: str
    token: str


def normalize_optional_phone(raw: str | None, field: str) -> str | None:
    if raw is None or not raw.strip():
        return None
    try:
        return externals_store.normalize_phone(raw)
    except ValueError:
        raise HTTPException(status_code=422, detail=f"{field} is not a valid phone number") from None


@router.post("/pair")
def pair(req: PairRequest, request: Request) -> PairResponse:
    """Decisions 2, 3 and O1 (revised): consume the code, mint the token,
    store the accepted numbers and give the owner the SIM (else Voice)
    number as `smsNumber`."""
    _check_webhook_ip_rate_limit(request, "bridge")
    if not rate_limits_store.check_and_increment(
        "bridge_pair:global", limit=PAIR_GLOBAL_LIMIT, window_s=PAIR_GLOBAL_WINDOW_S
    ):
        raise HTTPException(status_code=429, detail="too many requests")
    sim = normalize_optional_phone(req.simNumber, "simNumber")
    voice = normalize_optional_phone(req.voiceNumber, "voiceNumber")

    bridge_id = bridges_store.consume_pair_code(req.code)
    bridge = bridges_store.get(bridge_id) if bridge_id else None
    if bridge is None or bridge.paired:
        raise HTTPException(status_code=404, detail="unknown or expired code")

    token, token_hash = mint_token(bridge.id)
    caps = bridge_numbers.caps_for(req.caps.sms, req.caps.gchat, sim, voice)
    status = {
        "accounts": req.accounts,
        "simNumber": sim,
        "voiceNumber": voice,
        "version": req.version,
    }
    bridges_store.set_token_hash(bridge.id, token_hash, caps=caps, status=status)
    bridges_store.set_numbers(bridge.id, sim_number=sim, voice_number=voice, caps=caps)
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    bridge_numbers.apply_numbers(fresh, request.app.state.broker)
    logger.info("bridge paired bridge=%s owner=%s sim=%s voice=%s", bridge.id, bridge.ownerUid,
                bool(sim), bool(voice))
    return PairResponse(bridgeId=bridge.id, token=token)


class HeartbeatRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    status: dict = Field(default_factory=dict)
    fcmToken: str | None = Field(default=None, max_length=4096)


SIM_CHANGED = "SIM changed"


@router.post("/heartbeat")
def heartbeat(
    req: HeartbeatRequest, bridge: Annotated[bridges_store.Bridge, Depends(require_bridge)]
) -> dict:
    """Decision 3: records the status; a reported SIM that differs from the
    accepted one sets `status.error` and nothing else (never `smsNumber`)."""
    status = dict(req.status)
    for key in ("simNumber", "voiceNumber"):
        value = status.get(key)
        if isinstance(value, str) and value.strip():
            try:
                status[key] = externals_store.normalize_phone(value)
            except ValueError:
                status[key] = None
        elif key in status:
            status[key] = None
    bridges_store.touch(bridge.id, status, req.fcmToken)
    reported = status.get("simNumber")
    if reported is not None and reported != bridge.simNumber:
        bridges_store.set_error(bridge.id, SIM_CHANGED)
    elif bridge.status.error == SIM_CHANGED:
        bridges_store.set_error(bridge.id, None)
    return {"pending": bridge_outbox.count_pending(bridge.id)}



OUTBOX_WAIT_CAP_S = 25
OUTBOX_POLL_INTERVAL_S = 2


@router.get("/outbox")
async def get_outbox(
    bridge: Annotated[bridges_store.Bridge, Depends(require_bridge)],
    wait: Annotated[int, Query(ge=0)] = 0,
) -> dict:
    """Decision 11 / O2: pending items, oldest first, at most 20. `wait`
    (cap 25 s, default 0) long-polls in 2-s Firestore reads; the sleep is
    `asyncio.sleep`, so a waiting poll holds no threadpool thread. The phone
    itself polls with `wait=0` (O2); `wait` is for the simulator and e2e."""
    deadline = time.monotonic() + min(wait, OUTBOX_WAIT_CAP_S)
    while True:
        items = await run_in_threadpool(bridge_outbox.list_pending, bridge.id)
        if items or time.monotonic() >= deadline:
            return {"items": [i.for_phone() for i in items]}
        await asyncio.sleep(min(OUTBOX_POLL_INTERVAL_S, max(0.0, deadline - time.monotonic())))


class AckRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    state: Literal["sent", "failed"]
    reason: str | None = Field(default=None, max_length=200)
    tier: Literal[1, 2] = 1


def apply_ack_to_delivery(item: bridge_outbox.OutboxItem) -> None:
    """Decision 11: re-applies the delivery transition from the stored row
    state (both helpers are monotonic), so a retried ack repairs a lost
    delivery write. Items without a `msgId` (hints, inspect) have none."""
    if not item.msgId or not item.bid:
        return
    if item.state == "sent":
        messages_store.mark_delivery_sent_if_queued(item.msgId, item.bid)
    elif item.state == "failed":
        messages_store.mark_delivery_failed_if_queued(item.msgId, item.bid, error=item.reason)


@router.post("/outbox/{ob_id}/ack", status_code=204)
def ack_outbox(
    ob_id: str, req: AckRequest, bridge: Annotated[bridges_store.Bridge, Depends(require_bridge)]
) -> Response:
    acked = bridge_outbox.ack(bridge.id, ob_id, req.state, req.reason, req.tier)
    if acked is None:
        raise HTTPException(status_code=404, detail="no such outbox item")
    item, transitioned = acked
    apply_ack_to_delivery(item)
    if transitioned and req.tier == 2:
        bridges_store.increment_tier2(bridge.id)
    latency_ms = -1
    if item.createdAt is not None and item.ackedAt is not None:
        latency_ms = int((item.ackedAt - item.createdAt).total_seconds() * 1000)
    logger.info(
        "bridge out bridge=%s ob=%s state=%s tier=%d ms=%d",
        bridge.id, ob_id, item.state, req.tier, latency_ms,
    )
    return Response(status_code=204)


# ---------------------------------------------------------------------------
# POST /bridge/events -- decision 5
# ---------------------------------------------------------------------------

EVENT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
EVENTS_PER_MINUTE = 120
MAX_EVENTS_PER_BATCH = 50


class EventConversation(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str = Field(min_length=1)
    title: str | None = Field(default=None, max_length=200)
    isGroup: bool = False
    link: str | None = Field(default=None, max_length=2048)

    @field_validator("id")
    @classmethod
    def _id_bytes(cls, v: str) -> str:
        if len(v.encode("utf-8")) > 512:
            raise ValueError("conversation.id longer than 512 bytes")
        return v


class EventSender(BaseModel):
    model_config = ConfigDict(extra="ignore")

    name: str = Field(default="", max_length=200)
    phone: str | None = Field(default=None, max_length=64)


class EventAttachment(BaseModel):
    model_config = ConfigDict(extra="ignore")

    kind: Literal["image", "video", "audio", "file"]


class BridgeEvent(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    source: Literal["sms", "gchat", "gvoice"]
    kind: Literal["message", "inspect"] = "message"
    conversation: EventConversation
    sender: EventSender = Field(default_factory=EventSender)
    text: str = Field(default="", max_length=1600)
    ts: float = 0
    attachments: list[EventAttachment] = Field(default_factory=list, max_length=20)
    people: list[Annotated[str, Field(max_length=200)]] = Field(default_factory=list, max_length=64)

    @field_validator("id")
    @classmethod
    def _id_shape(cls, v: str) -> str:
        if not EVENT_ID_RE.match(v):
            raise ValueError("event id must match ^[A-Za-z0-9_-]{1,64}$")
        return v


class EventsRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")

    events: list[BridgeEvent] = Field(max_length=MAX_EVENTS_PER_BATCH)


def conv_log_id(conversation_id: str) -> str:
    return hashlib.sha256(conversation_id.encode()).hexdigest()[:8]


def _from_label(event: BridgeEvent) -> str:
    if event.sender.phone:
        return sms_client.redact_phone(event.sender.phone)
    return event.sender.name or "-"


def _handle_text_event(
    bridge: bridges_store.Bridge, event: BridgeEvent, routing
) -> str:
    """`sms` / `gvoice` messages (decision 5): the target is always the
    bridge owner; the step table is `inbound_text.handle_text`."""
    if event.kind != "message":
        return "dropped_unsupported"
    if event.conversation.isGroup:
        return "dropped_group"
    target = users_store.get_user(bridge.ownerUid)
    if (
        target is None
        or target.kind != "person"
        or target.disabled
        or target.familyId is None
    ):
        return "dropped_owner"
    caps_ok = bridge.caps.sms if event.source == "sms" else bridge.caps.gvoice
    if not caps_ok:
        return "dropped_cap"
    try:
        from_number = externals_store.normalize_phone(event.sender.phone or "")
    except ValueError:
        return "dropped_bad_from"
    sid = f"br_{bridge.id}_{event.id}"
    gvoice = event.source == "gvoice"

    def reply(text: str) -> None:
        bridge_outbox.enqueue_hint(
            bridge,
            source=event.source,
            phone=from_number,
            text=text,
            wire_id=sid,
            to_extra=(
                {"conversationId": event.conversation.id, "link": voice_link(from_number)}
                if gvoice
                else None
            ),
        )

    return handle_text(
        target,
        from_number,
        event.text,
        sid,
        routing,
        reply=reply,
        attachments=[a.kind for a in event.attachments],
        via=event.source,
        voice_conv=event.conversation.id if gvoice else None,
    )


CONVERSATIONS_PER_DAY = 50


def _preview(event: BridgeEvent) -> str:
    return (event.text.strip() or (placeholder_for([a.kind for a in event.attachments]) if event.attachments else ""))


def _handle_chat_event(
    bridge: bridges_store.Bridge, event: BridgeEvent, routing, broker
) -> str:
    """`gchat` events (decisions 5, 6, 8)."""
    conv = event.conversation
    existing = bridge_conversations.get(bridge.id, conv.id)
    if event.kind == "inspect":
        bridge_conversations.upsert_seen(
            bridge,
            source=event.source,
            conversation_id=conv.id,
            title=conv.title,
            is_group=conv.isGroup,
            link=conv.link,
            speaker=None,
            people=event.people,
            preview=None,
            inspected=True,
        )
        return "inspected"
    if existing is not None and existing.status == "ignored":
        return "dropped_ignored"
    if existing is not None and existing.status == "paused":
        return "dropped_paused"
    target = users_store.get_user(bridge.ownerUid)
    if target is None or target.kind != "person" or target.disabled or target.familyId is None:
        return "dropped_owner"
    if not bridge.caps.gchat:
        return "dropped_cap"
    body = _preview(event)
    if not body:
        return "dropped_empty"
    if existing is None:
        since = datetime.now(UTC) - timedelta(days=1)
        if bridge_conversations.count_created_since(bridge.id, since) >= CONVERSATIONS_PER_DAY:
            return "dropped_conv_cap"
    row = bridge_conversations.upsert_seen(
        bridge,
        source=event.source,
        conversation_id=conv.id,
        title=conv.title,
        is_group=conv.isGroup,
        link=conv.link,
        speaker=event.sender.name,
        people=event.people,
        preview=body,
    )
    if row.status == "subscribed":
        outcome = chat_subscribe.deliver_subscribed(
            bridge, row, event, routing, broker, existing.title if existing else None
        )
        if outcome is not None:
            return outcome
    hid = f"br_{bridge.id}_{event.id}"
    if held_chat_store.count_held(row.id) >= held_chat_store.HELD_CAP:
        return "held_cap"
    if not held_chat_store.create(
        hid,
        bridge_id=bridge.id,
        conversation_id=conv.id,
        conv_row_id=row.id,
        family_id=target.familyId,
        to_uid=target.uid,
        sender_name=event.sender.name,
        body=body,
    ):
        return "duplicate"
    alert_id = alerts_module.chat_held_upsert(
        target.familyId, target, row, body, event.sender.name or "someone"
    )
    bridge_conversations.set_fields(
        row.id, heldCount=held_chat_store.count_held(row.id), alertId=alert_id
    )
    return "held"


def process_event(bridge: bridges_store.Bridge, event: BridgeEvent, routing, broker) -> str:
    if event.source in ("sms", "gvoice") and event.kind == "message":
        return _handle_text_event(bridge, event, routing)
    if event.source in ("gchat", "gvoice"):
        return _handle_chat_event(bridge, event, routing, broker)
    return "dropped_unsupported"


def process_events(
    bridge: bridges_store.Bridge, events: list[BridgeEvent], routing, broker
) -> list[dict]:
    for _ in events:
        if not rate_limits_store.check_and_increment(
            f"bridge_events:{bridge.id}", limit=EVENTS_PER_MINUTE, window_s=60
        ):
            raise HTTPException(status_code=429, detail="too many requests")
    results = []
    for event in events:
        outcome = process_event(bridge, event, routing, broker)
        logger.info(
            "bridge in bridge=%s src=%s conv=%s from=%s outcome=%s",
            bridge.id, event.source, conv_log_id(event.conversation.id), _from_label(event), outcome,
        )
        results.append({"id": event.id, "outcome": outcome})
    return results


@router.post("/events")
async def post_events(
    req: EventsRequest,
    request: Request,
    bridge: Annotated[bridges_store.Bridge, Depends(require_bridge)],
) -> dict:
    results = await run_in_threadpool(
        process_events, bridge, req.events, request.app.state.routing, request.app.state.broker
    )
    return {"results": results}

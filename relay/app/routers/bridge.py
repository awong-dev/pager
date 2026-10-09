"""`/bridge/*` -- the headless bridge phone's API (docs/BRIDGE_PHONE_DESIGN.md).

`POST /bridge/pair` is unauthenticated (the pairing code is the credential);
every other route authenticates `Authorization: Bearer <bridgeId>.<secret>`
through `app.bridgeauth.require_bridge`.
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response
from pydantic import BaseModel, ConfigDict, Field
from starlette.concurrency import run_in_threadpool

from app import bridge_numbers
from app.bridgeauth import mint_token, require_bridge
from app.routers.webhooks import _check_webhook_ip_rate_limit
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import externals as externals_store
from app.store import messages as messages_store
from app.store import rate_limits as rate_limits_store

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

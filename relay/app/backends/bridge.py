"""`bridge` backend -- docs/BRIDGE_PHONE_DESIGN.md decisions 4, 7, 11.

The row lives on a subscribed Google Chat / Google Voice conversation (an
external with `chat` set). `deliver()` hands the text to the bridge phone's
outbox and leaves the delivery `queued`; the phone's ack
(`POST /bridge/outbox/{id}/ack`) moves it to `sent` or `failed`.
`apply_outbox_state` is shared with `sms_twilio`'s bridge transport.
"""

from __future__ import annotations

import logging

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.store import bridge_conversations, bridge_outbox
from app.store import bridges as bridges_store
from app.store import messages as messages_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.bridge")


class BridgeConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    bridgeId: str
    source: str = "gchat"
    conversationId: str | None = None
    link: str | None = None


def apply_outbox_state(
    msg: Message, backend_id: str, item: bridge_outbox.OutboxItem
) -> DeliverResult:
    """Decision 4: the outbox row's state decides the delivery's, so a
    redeliver also repairs an ack whose delivery write was lost."""
    messages_store.set_delivery_external_id(msg.id, backend_id, item.id)
    if item.state == "sent":
        messages_store.mark_delivery_sent_if_queued(msg.id, backend_id)
        return DeliverResult(ok=True, state="sent", external_id=item.id)
    if item.state == "failed":
        messages_store.mark_delivery_failed_if_queued(msg.id, backend_id, error=item.reason)
        return DeliverResult(ok=False, state="failed", external_id=item.id, error=item.reason)
    return DeliverResult(ok=True, state="queued", external_id=item.id)


class BridgeBackend:
    kind = "bridge"
    config_schema = BridgeConfig

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        config = BridgeConfig.model_validate(backend.config)
        bridge = bridges_store.get(config.bridgeId)
        if bridge is None or not bridge.paired:
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id, error="no_bridge")
            return DeliverResult(ok=False, state="failed", error="no_bridge")
        if config.conversationId:
            row = bridge_conversations.get(bridge.id, config.conversationId)
            if row is not None and row.status == "paused":
                messages_store.mark_delivery_failed_if_queued(msg.id, backend.id, error="paused")
                return DeliverResult(ok=False, state="failed", error="paused")
        if msg.kind != "text" or not msg.body:
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id, error="unsupported")
            return DeliverResult(ok=False, state="failed", error="unsupported")
        item = bridge_outbox.enqueue_send(
            bridge,
            msg.id,
            backend.id,
            source=config.source,
            to={"conversationId": config.conversationId, "link": config.link},
            text=msg.body,
            reply_hint=config.conversationId,
        )
        logger.info(
            "bridge enqueue bridge=%s ob=%s src=%s state=%s", bridge.id, item.id, config.source, item.state
        )
        return apply_outbox_state(msg, backend.id, item)

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return False

    def render_state(self, delivery: Delivery) -> str:
        if delivery.state == "queued":
            return "waiting for the phone"
        if delivery.state == "sent":
            return "sent on Google Chat"
        return delivery.state

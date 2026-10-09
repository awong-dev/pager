"""`sms` backend -- docs/BRIDGE_PHONE_DESIGN.md decision 4 (+ O1 revised).

SMS exists only through bridge phones. The row lives on an *external* (an SMS
contact); `To` = `backend.config["phone"]`, and the sender's `users.smsNumber`
(set by the relay when a bridge pairs, `app/bridge_numbers.py`) must be a
paired bridge's SIM or Voice number: the send is enqueued on that bridge's
outbox (`app/store/bridge_outbox.py`) and the phone acks it. A sender without
a number fails at once (`no_sms_number`); a number that is not a usable
bridge number fails `no_bridge`. Neither is ever retried. Inbound texts
arrive via `/bridge/events` (`app/inbound_text.py`), not here. No link flow.

The earlier relay-number SMS transport was removed after commit 05ec3ed.
"""

from __future__ import annotations

import logging

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.backends.bridge import apply_outbox_state
from app.sms_text import redact_phone, voice_link
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.sms")

LOC_PREVIEW = "location"


class SmsConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    phone: str


def _render_body(msg: Message) -> str:
    """Outbound rendering. `kind='loc'` has no `body`, only `loc`; a short
    fixed preview stands in for a real "lat,lon" rendering."""
    if msg.body:
        return msg.body
    if msg.kind == "loc" and msg.loc is not None:
        return LOC_PREVIEW
    return "(no content)"


class SmsBackend:
    kind = "sms"
    config_schema = SmsConfig

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        phone = backend.config.get("phone")
        if not phone:
            logger.warning("sms backend %s has no phone configured", backend.id)
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id)
            return DeliverResult(ok=False, state="failed", error="sms backend missing phone")

        sender = users_store.get_user(msg.senderUid)
        from_number = sender.smsNumber if sender is not None else None
        if not from_number:
            logger.info(
                "sms out to=%s from=- sid=- status=failed code=no_sms_number",
                redact_phone(phone),
            )
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id)
            return DeliverResult(ok=False, state="failed", error="no_sms_number")

        # docs/BRIDGE_PHONE_DESIGN.md decision 4 (+ O1 revised): SMS exists only
        # through a paired bridge phone. The sender's number must be that
        # bridge's SIM or Voice number and the phone must be able to send
        # something right now; otherwise the send fails, never retried.
        bridge = bridges_store.get_by_sms_number(from_number)
        if bridge is None or not (bridge.caps.sms or bridge.caps.gvoice):
            logger.info(
                "sms out to=%s from=%s sid=- status=failed code=no_bridge",
                redact_phone(phone),
                redact_phone(from_number),
            )
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id, error="no_bridge")
            return DeliverResult(ok=False, state="failed", error="no_bridge")
        return self._deliver_via_bridge(msg, backend, bridge, phone, from_number)

    def _deliver_via_bridge(
        self,
        msg: Message,
        backend: BackendRow,
        bridge: bridges_store.Bridge,
        phone: str,
        from_number: str,
    ) -> DeliverResult:
        """Channel per send: the sender's `config.via` entry for this
        external if set, else `gvoice` when the sender's number is the
        bridge's Voice number (and not its SIM), else `sms`; demoted to
        whatever the bridge's caps allow."""
        via = (backend.config.get("via") or {}).get(msg.senderUid)
        if via not in ("sms", "gvoice"):
            via = "gvoice" if from_number == bridge.voiceNumber != bridge.simNumber else "sms"
        if via == "gvoice" and not bridge.caps.gvoice:
            via = "sms"
        if via == "sms" and not bridge.caps.sms:
            via = "gvoice"
        to: dict[str, str] = {"phone": phone}
        if via == "gvoice":
            conv = (backend.config.get("voiceConv") or {}).get(msg.senderUid)
            if conv:
                to["conversationId"] = conv
            to["link"] = voice_link(phone)
        item = bridge_outbox.enqueue_send(
            bridge, msg.id, backend.id, source=via, to=to, text=_render_body(msg)
        )
        logger.info(
            "sms out to=%s from=%s sid=%s status=%s code=bridge",
            redact_phone(phone),
            redact_phone(from_number),
            item.id,
            "queued" if item.state == "pending" else item.state,
        )
        return apply_outbox_state(msg, backend.id, item)

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return False

    def render_state(self, delivery: Delivery) -> str:
        return "sent by SMS" if delivery.state == "sent" else delivery.state

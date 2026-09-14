"""`pager` backend -- the MQTT gateway, now over the broker's REST publish
API. docs/SERVER_PLAN.md §6.2.

`deliver()` builds the §3.2 down envelope (`from` = sender's alias, or
`system` as a defensive fallback that should never actually trigger since
`routing.py` only ever creates a message for a real registered
`senderUid`) and calls `BrokerClient.publish(topic, payload, qos=1,
retain=False)`; a 2xx marks the delivery `sent`. A non-2xx leaves it
`queued` -- `/internal/tick` (`app/jobs.py`) retries it later.

The down envelope's wire `id` is the Firestore message id itself (`m_` + 8
hex, which already satisfies PROTOCOL.md §1's generic `id` shape) -- reusing
it rather than minting a second id is what lets the online-edge republish
(`app/ingest.py`) resend the *same* `id` per PROTOCOL.md §5.3, so the
device's own dedup ring (§4.1 rule 7) suppresses double-rendering.

`backend.config["deviceId"]` is where the fan-out (`app/routing.py`) and the
online-edge/`tick` retries get the target device id from; it is stashed a
second time in the delivery's own `externalId` (see
`app/store/messages.py`'s `find_pager_delivery`) so an inbound `/up` ack can
be matched back to this delivery without a second backend-doc read.
"""

from __future__ import annotations

import logging

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.broker import BrokerClient
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User
from app.wire import build_down_payload

logger = logging.getLogger("relay.backends.pager")

SYSTEM_ALIAS = "system"


class PagerConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    deviceId: str


class PagerBackend:
    kind = "pager"
    config_schema = PagerConfig

    def __init__(self, broker: BrokerClient) -> None:
        self._broker = broker

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        device_id = backend.config.get("deviceId")
        if not device_id:
            logger.warning("pager backend %s has no deviceId configured", backend.id)
            return DeliverResult(ok=False, state="failed", error="pager backend missing deviceId")

        down_kind = "loc_req" if msg.kind == "loc_req" else "msg"
        payload = build_down_payload(
            msg_id=msg.id,
            ts=msg.ts,
            body=msg.body,
            kind=down_kind,
            from_=self._sender_alias(msg.senderUid),
        )
        ok = self._broker.publish(f"pager/{device_id}/down", payload, qos=1, retain=False)
        if ok:
            messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
            return DeliverResult(ok=True, state="sent", external_id=device_id)
        return DeliverResult(ok=False, state="queued", error="broker publish failed")

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        # Pager backends are provisioned by `POST /api/admin/devices`
        # (admin flow), not a per-user link flow -- nothing for the user to
        # do here.
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return False

    def render_state(self, delivery: Delivery) -> str:
        return delivery.state

    @staticmethod
    def _sender_alias(sender_uid: str) -> str:
        user = users_store.get_user(sender_uid)
        return user.alias if user is not None else SYSTEM_ALIAS

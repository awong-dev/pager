"""`pager` backend -- the MQTT gateway, now over the broker's REST publish
API. docs/SERVER_PLAN.md §6.2.

`deliver()` builds the §3.2 down envelope's fields (`from` = sender's alias,
or `system` as a defensive fallback that should never actually trigger since
`routing.py` only ever creates a message for a real registered
`senderUid`) and hands them to `BrokerClient.publish_down(device_id, obj)`
(docs/DEVICE_TASKS.md S1.4) -- the one place that decides the wire encoding,
takes a fresh `n`, signs (or not) per `devices/{d}.authMode`, and calls
`publish()`; a 2xx marks the delivery `sent`. A non-2xx (or `publish_down`
itself declining, e.g. an `authMode: "hmac"` device with no `deviceSecrets`
row) leaves it `queued` -- `/internal/tick` (`app/jobs.py`) retries it
later, which is also how the online-edge republish (`app/routing.py`'s
`redeliver_pager`, called from `app/ingest.py`) reaches this same code path
and gets a fresh `n` for the same reused `id` (PROTOCOL.md §5.3).

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
from typing import Any

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.broker import BrokerClient
from app.store import conversations as conversations_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.pager")

SYSTEM_ALIAS = "system"


def _build_down_obj(
    *,
    msg_id: str,
    ts: int,
    kind: str,
    from_: str,
    body: str | None,
    sndr: str | None = None,
) -> dict[str, Any]:
    """The §3.1/§3.2 down envelope's fields, as a plain dict for
    `BrokerClient.publish_down` (which adds `n`/`sig` itself, per S1.4).
    Mirrors `wire.build_down_payload`'s own field-omission rules (`kind`
    omitted when it is the default `msg`; `loc_req` never carries a `body`;
    `ack` is always present and `None`) without depending on that function,
    which returns pre-serialised JSON bytes rather than a dict and is
    outside this task's `Files` list -- see docs/DEVICE_TASKS.md S1.4.

    `sndr` (docs/GROUP_CHAT_DESIGN.md §4) is the group-message author's
    alias, in the field-order slot `to` would occupy on an up envelope
    (§3.1's "SHOULD emit ... from,to,sndr,body,ack ..."); omitted entirely
    when `None` (every DM page, and every `loc_req`), so a DM page's bytes
    are unaffected by this field's existence."""
    obj: dict[str, Any] = {"v": 1, "id": msg_id, "ts": ts}
    if kind != "msg":
        obj["kind"] = kind
    obj["from"] = from_
    if sndr is not None:
        obj["sndr"] = sndr
    if kind != "loc_req":
        obj["body"] = body
    obj["ack"] = None
    return obj


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
        from_alias, sndr_alias = self._from_and_sndr(msg, down_kind)
        obj = _build_down_obj(
            msg_id=msg.id,
            ts=msg.ts,
            kind=down_kind,
            from_=from_alias,
            body=msg.body,
            sndr=sndr_alias,
        )
        ok = self._broker.publish_down(device_id, obj)
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

    @classmethod
    def _from_and_sndr(cls, msg: Message, down_kind: str) -> tuple[str, str | None]:
        """docs/GROUP_CHAT_DESIGN.md §4: on a group copy (`msg.groupMsgId`
        set), `from` becomes the *group's* alias -- the thread identity the
        device's address book and reply both key on -- and `sndr` is the
        author, taken straight from `msg.senderAlias` (denormalised
        specifically so this never needs a `users/{uid}` read per delivery,
        see `app/store/messages.py`'s `Message.senderAlias` docstring). A DM
        copy (`groupMsgId` absent, the overwhelming common case) is
        unaffected: `from` is the sender's own alias and `sndr` is `None`,
        byte-identical to every page built before this field existed. Never
        set on a `loc_req` (group `/locate` is out of scope) even if a
        caller somehow got here with one -- defensive, since `_send_group`
        never sends a `loc_req`."""
        if msg.groupMsgId is None or down_kind != "msg":
            return cls._sender_alias(msg.senderUid), None
        conv = conversations_store.get_conversation(msg.convKey)
        if conv is not None and conv.alias is not None:
            return conv.alias, msg.senderAlias
        return cls._sender_alias(msg.senderUid), msg.senderAlias

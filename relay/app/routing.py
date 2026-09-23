"""The routing engine -- docs/SERVER_PLAN.md §5.2, §5.4.

```
send(sender, recipient_alias | None, kind, body, origin_backend, wire_id=None):
  1. resolve recipients: explicit alias -> [uid] ; None -> device default or broadcast set
  2. for each recipient: assert allow/{sender}_{recipient}.message else drop+log+system-reply
  3. one Firestore transaction per recipient: create wireIds/{wire}_{recipient} (dedup; aborts
     if present), increment seqCounter, create messages/{id} with a 'queued' delivery per enabled
     backend (excluding origin_backend) and pendingDeviceIds, update conversations/{convKey}
  4. deliver inline, in-request: pager -> broker REST publish (-> 'sent' on 2xx); webapp -> FCM;
     sms/gchat -> provider call. Any failure leaves the delivery 'queued'/'failed' ...
```

Two deliberate, minimal shapes not spelled out by that shorthand signature:

- **`origin_backend` is split into `origin_backend_kind` (always given) and
  `origin_backend_id` (`None` for `pager`/`webapp`)**. §6.1's "adapters ...
  pass their own backend row as origin" implies a specific
  `users/{uid}/backends/{bid}` document id, but `pager` and `webapp` sends
  have no per-*user* backend row standing for "the channel this arrived on"
  (a device's pager backend belongs to its *owner*, not necessarily the
  sender; webapp has no row concept distinct from the user at all), so both
  pass `origin_backend_id=None` and only the kind is known. It matters for
  SMS/gchat: a user with two backends of the same kind (two phones) needs
  "reply back through the channel it arrived on" to name the *specific*
  backend, which the kind alone cannot express. Carrying both fields
  (`messages/{id}.originBackendKind`/`.originBackendId`) means an adapter
  that does know its row can pass a real id. See `docs/SERVER_PLAN.md`
  §3/§5.2 for the field shapes; nothing here is device-visible.

  "Excluding origin backend" (§5.2's closing paragraph) is a **self-loop
  guard**, not a blanket "never deliver this kind to this recipient" rule:
  a backend is excluded from the recipient's fan-out *only when
  `recipient_uid == sender_uid`* -- i.e. only when this particular send
  would otherwise hand the message straight back to the same channel it
  just arrived on for the same person. Every worked example the spec gives
  ("stops an SMS reply from being echoed back to the same phone", "doesn't
  re-queue a pager delivery back to the sender's own device") is phrased
  around "the same phone"/"the sender's own device" -- i.e. the sender
  receiving their own message back, not a *different* recipient losing a
  delivery of a kind they happen to share with the sender. When
  `origin_backend_id` is given, the guard excludes by that exact id (so a
  same-kind, different *backend* sibling is never excluded); when it is
  `None` (the pager/webapp callers), it falls back to the kind-based guard
  below.

  This scoping matters: a literal "exclude every recipient's same-kind
  backend" reading breaks the ordinary case. A `webapp`-originated send (the
  ordinary parent<->student case, `origin_backend_kind="webapp"`) would
  exclude the recipient's own implicit `webapp` backend (same kind), leaving
  the message with no `webapp` delivery entry at all -- no FCM push, and
  `POST /api/conversations/{alias}/messages/{id}/read` (§6.3) 404s because
  there is nothing to mark read. Since `recipient_uid` can never
  legitimately equal `sender_uid` in
  normal operation (alias resolution and the broadcast set both exclude the
  sender), this guard is a no-op in the common case and only bites the
  degenerate/misconfigured case (e.g. a self-referential allow edge, or a
  future SMS/gchat inbound-resolution bug that resolves a reply back to its
  own sender) -- which is exactly the case "the sender's own device"/"the
  same phone" describes. `app/backends/pager.py`'s module docstring and
  `tests/test_routing.py` exercise both the guard (self-loop) and the
  normal cross-user case (recipient keeps every enabled backend regardless
  of the sender's channel).
- **An optional `device_id` keyword** is added for the one case the
  shorthand signature elides: resolving "device default" when
  `recipient_alias` is `None`. `devices/{deviceId}.defaultToUid` is stored
  per-*device*, not per-*user* (a sender could in principle own more than
  one), so *which* device's default applies has to come from somewhere --
  `app/ingest.py` already knows `device_id` (it parsed it off the webhook
  topic) and passes it through. Webapp-originated sends never pass one
  (they always carry an explicit `recipient_alias`, so this branch never
  runs for them).

Step 3's per-recipient transaction is `app/store/messages.py`'s
`create_message`; this module supplies the allow-list gate and the
delivery/pendingDeviceIds construction around it.
Step 4 (inline delivery) calls straight into `app/backends/*` through the
kind -> Backend registry (`app/backends/registry.py`); each backend module
owns its own delivery-state transaction (see `backends/base.py`'s
docstring), so this module never touches `deliveries.*` directly.

**Group fan-out** (docs/GROUP_CHAT_DESIGN.md §3): `send()` checks, before
any of the above, whether `recipient_alias` resolves to a *group*
conversation (`app/store/conversations.py:get_by_alias`) rather than a user.
If so, `_send_group` takes over entirely: recipients = the group's members
minus the sender (403-equivalent `RejectedRecipient(reason="not_member")` if
the sender isn't one of them), one `allocate_seq()` call mints a `seq`
shared by every copy, and every per-recipient `create_message` call carries
the group's `convKey`, that one `seq`, a shared `groupMsgId` and the
sender's alias. The allow-list gate still runs per recipient exactly as in
the DM path -- **group membership is not a substitute for an allow edge**
(docs/GROUP_CHAT_DESIGN.md §3 step 5): a missing edge drops that one
recipient (logged `SECURITY`) without touching the others. The self-loop
guard in `_create_and_deliver` is exercised the same way it always is (and
stays a no-op here, since the sender is already excluded from the recipient
set).

`groupMsgId`: when this call carries a `wire_id` (a device-originated `/up`
with `to: <group alias>`, which already has one -- its own envelope id),
`groupMsgId` is set *to that same value* rather than a freshly minted one.
This is a deliberate reading of docs/GROUP_CHAT_DESIGN.md §3's "device sends
already carry a wireId" beyond what it says outright: minting a separate
random `groupMsgId` on every call would mean a QoS-1 redelivery that repairs
a partial fan-out mints a *different* `groupMsgId` for the copies it
creates than the ones that already landed on the first attempt, breaking
"one id shared by every copy of one logical group message" (§2) for exactly
the retry case §3's "Failure modes" section says this is supposed to repair.
Reusing `wire_id` makes a device-originated group send's `groupMsgId`
idempotent under redelivery, matching its message-document dedup (`wireIds`)
for free. A web-originated send (no incoming `wire_id`) mints a fresh
`groupMsgId` and, per §3's text, uses it as the per-recipient `wireIds`
dedup key too -- see `_send_group`'s body for exactly where.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Literal

from app.backends.base import Backend, DeliverResult
from app.backends.registry import build_registry
from app.broker import BrokerClient
from app.ids import new_id
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.backends import Backend as BackendRow
from app.store.conversations import Conversation
from app.store.messages import Delivery, Message, MessageKind

logger = logging.getLogger("relay.routing")

RejectReason = Literal["unknown_alias", "not_allowed", "not_member"]


@dataclass(frozen=True, slots=True)
class RejectedRecipient:
    alias: str | None
    uid: str | None
    reason: RejectReason


@dataclass(frozen=True, slots=True)
class SendResult:
    messages: list[Message] = field(default_factory=list)
    rejected: list[RejectedRecipient] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return bool(self.messages) and not self.rejected


class Routing:
    """Holds the broker (for the backend registry) and, optionally, a
    pre-built registry -- same injectable-collaborator shape as
    `app.ingest.Ingest`, so tests can pass a `FakeBrokerClient` or a fake
    registry without touching real MQTT/FCM."""

    def __init__(self, broker: BrokerClient, registry: dict[str, Backend] | None = None) -> None:
        self._broker = broker
        self._registry = registry if registry is not None else build_registry(broker)

    def send(
        self,
        *,
        sender_uid: str,
        recipient_alias: str | None,
        kind: MessageKind,
        origin_backend_kind: str,
        origin_backend_id: str | None = None,
        body: str | None = None,
        loc: dict | None = None,
        wire_id: str | None = None,
        device_id: str | None = None,
        ts: int | None = None,
    ) -> SendResult:
        ts = ts if ts is not None else int(time.time())

        if recipient_alias is not None:
            group = conversations_store.get_by_alias(recipient_alias)
            if group is not None:
                return self._send_group(
                    sender_uid=sender_uid,
                    group=group,
                    kind=kind,
                    body=body,
                    loc=loc,
                    origin_backend_kind=origin_backend_kind,
                    origin_backend_id=origin_backend_id,
                    wire_id=wire_id,
                    ts=ts,
                )

        candidates, rejected = self._candidate_recipients(sender_uid, recipient_alias, device_id)

        created: list[Message] = []
        for recipient_uid in candidates:
            if not allow_store.is_message_allowed(sender_uid, recipient_uid):
                logger.warning(
                    "SECURITY sender %s not allowed to message recipient %s (alias=%r)",
                    sender_uid,
                    recipient_uid,
                    recipient_alias,
                )
                rejected.append(
                    RejectedRecipient(alias=recipient_alias, uid=recipient_uid, reason="not_allowed")
                )
                continue
            msg = self._create_and_deliver(
                sender_uid=sender_uid,
                recipient_uid=recipient_uid,
                kind=kind,
                body=body,
                loc=loc,
                origin_backend_kind=origin_backend_kind,
                origin_backend_id=origin_backend_id,
                wire_id=wire_id,
                ts=ts,
            )
            if msg is not None:
                created.append(msg)
        return SendResult(messages=created, rejected=rejected)

    # ---- recipient resolution (§5.2 step 1) ----

    def _candidate_recipients(
        self, sender_uid: str, recipient_alias: str | None, device_id: str | None
    ) -> tuple[list[str], list[RejectedRecipient]]:
        if recipient_alias is not None:
            uid = users_store.get_uid_for_alias(recipient_alias)
            if uid is None:
                logger.warning(
                    "SECURITY unknown recipient alias %r from sender %s", recipient_alias, sender_uid
                )
                return [], [
                    RejectedRecipient(alias=recipient_alias, uid=None, reason="unknown_alias")
                ]
            return [uid], []

        default_uid: str | None = None
        if device_id is not None:
            device = devices_store.get_device(device_id)
            if device is not None:
                default_uid = device.defaultToUid
        if default_uid is not None:
            return [default_uid], []
        return allow_store.allowed_recipients(sender_uid), []

    # ---- group fan-out (docs/GROUP_CHAT_DESIGN.md §3) ----

    def _send_group(
        self,
        *,
        sender_uid: str,
        group: Conversation,
        kind: MessageKind,
        body: str | None,
        loc: dict | None,
        origin_backend_kind: str,
        origin_backend_id: str | None,
        wire_id: str | None,
        ts: int,
    ) -> SendResult:
        if sender_uid not in group.uids:
            logger.warning(
                "SECURITY sender %s is not a member of group %s (alias=%r)",
                sender_uid,
                group.convKey,
                group.alias,
            )
            return SendResult(
                messages=[],
                rejected=[
                    RejectedRecipient(alias=group.alias, uid=sender_uid, reason="not_member")
                ],
            )

        sender = users_store.get_user(sender_uid)
        sender_alias = sender.alias if sender is not None else sender_uid

        seq = messages_store.allocate_seq()
        # See this module's docstring for why a device-originated send
        # (wire_id already given) reuses it as groupMsgId rather than
        # minting a second, independent id.
        group_msg_id = wire_id if wire_id is not None else new_id("gm_")

        created: list[Message] = []
        rejected: list[RejectedRecipient] = []
        for recipient_uid in sorted(uid for uid in group.uids if uid != sender_uid):
            if not allow_store.is_message_allowed(sender_uid, recipient_uid):
                logger.warning(
                    "SECURITY sender %s not allowed to message group member %s (group=%s)",
                    sender_uid,
                    recipient_uid,
                    group.convKey,
                )
                rejected.append(
                    RejectedRecipient(alias=group.alias, uid=recipient_uid, reason="not_allowed")
                )
                continue
            msg = self._create_and_deliver(
                sender_uid=sender_uid,
                recipient_uid=recipient_uid,
                kind=kind,
                body=body,
                loc=loc,
                origin_backend_kind=origin_backend_kind,
                origin_backend_id=origin_backend_id,
                wire_id=group_msg_id,
                ts=ts,
                conv_key=group.convKey,
                seq=seq,
                group_msg_id=group_msg_id,
                sender_alias=sender_alias,
            )
            if msg is not None:
                created.append(msg)
        return SendResult(messages=created, rejected=rejected)

    def redeliver_pager(self, msg: Message, device_id: str) -> bool:
        """Re-invokes the `pager` backend's `deliver()` for `msg`'s delivery
        addressed to `device_id` -- the shared primitive behind both the
        online-edge republish (`app/ingest.py`, PROTOCOL.md §5.3) and
        `/internal/tick`'s retry of still-`queued` pager deliveries
        (docs/SERVER_PLAN.md §5.8 item 1). Returns False (nothing done) if
        `msg` has no pager delivery for this device, its backend doc is
        gone, or no `pager` implementation is registered."""
        bid = messages_store.find_pager_delivery(msg, device_id)
        if bid is None:
            return False
        delivery = msg.deliveries.get(bid)
        backend_row = backends_store.get_backend(msg.recipientUid, bid)
        if delivery is None or backend_row is None:
            return False
        if self._registry.get("pager") is None:
            return False
        # Routed through `_deliver_one` (not `backend.deliver()` directly) so
        # a retry's outcome gets the same attempts/error/failed bookkeeping
        # as the initial inline delivery -- otherwise a permanently
        # failing pager delivery retried only through this path (tick,
        # online-edge) would never reach `failed` and would be retried
        # forever.
        self._deliver_one(msg, bid, delivery, backend_row)
        return True

    def redeliver(self, msg: Message, bid: str) -> bool:
        """Generic re-invocation of *any* backend's `deliver()` for delivery
        `bid` on `msg` -- `redeliver_pager`'s non-device-keyed sibling, so
        `/internal/tick`'s retry of queued non-pager deliveries
        (`app/jobs.py`, docs/SERVER_PLAN.md §5.2's general "any failure ...
        enqueues a retry" rule, applied to more than just `pager`) has
        something to call that doesn't need a device id the way
        `redeliver_pager` does. `redeliver_pager` stays as its
        own method rather than being rewritten in terms of this one: it is
        the hot, already-tested online-edge/`/locate` path, and its extra
        device-id lookup has no equivalent for a kind like `sms` that never
        populates `pendingDeviceIds`. Returns False if the delivery or its
        backend row is gone."""
        delivery = msg.deliveries.get(bid)
        if delivery is None:
            return False
        backend_row = backends_store.get_backend(msg.recipientUid, bid)
        if backend_row is None:
            return False
        if self._registry.get(backend_row.kind) is None:
            return False
        self._deliver_one(msg, bid, delivery, backend_row)
        return True

    # ---- create + deliver (§5.2 steps 3-4) ----

    def _create_and_deliver(
        self,
        *,
        sender_uid: str,
        recipient_uid: str,
        kind: MessageKind,
        body: str | None,
        loc: dict | None,
        origin_backend_kind: str,
        origin_backend_id: str | None,
        wire_id: str | None,
        ts: int,
        conv_key: str | None = None,
        seq: int | None = None,
        group_msg_id: str | None = None,
        sender_alias: str | None = None,
    ) -> Message | None:
        # Self-loop guard only (see this module's docstring for why it is
        # scoped to recipient_uid == sender_uid rather than a blanket
        # same-kind exclusion): in normal operation recipient_uid is never
        # sender_uid, so this never removes a legitimate delivery. When the
        # caller knows the *specific* origin backend document (the SMS/gchat
        # adapters, which pass a real per-user backend id -- see
        # this module's docstring on `origin_backend_id`), exclude by that
        # exact id instead of by kind, so a recipient who happens to share a
        # backend *kind* with the sender (e.g. two different phones) never
        # loses a legitimate delivery to this guard. Falls back to the
        # kind-based guard when no id is given (pager and webapp, which have
        # no per-user backend row for their own origin the way SMS and gchat
        # do).
        self_loop = recipient_uid == sender_uid
        enabled = [
            b
            for b in backends_store.list_backends(recipient_uid)
            if b.enabled
            and not (
                self_loop
                and (
                    b.id == origin_backend_id
                    if origin_backend_id is not None
                    else b.kind == origin_backend_kind
                )
            )
        ]

        deliveries: dict[str, dict] = {}
        pending_device_ids: list[str] = []
        for b in enabled:
            entry: dict[str, object] = {"kind": b.kind, "state": "queued", "attempts": 0}
            if b.kind == "pager":
                pager_device_id = b.config.get("deviceId")
                if not pager_device_id:
                    logger.warning("pager backend %s has no deviceId configured, skipping", b.id)
                    continue
                entry["externalId"] = pager_device_id
                pending_device_ids.append(pager_device_id)
            deliveries[b.id] = entry

        msg = messages_store.create_message(
            sender_uid=sender_uid,
            recipient_uid=recipient_uid,
            kind=kind,
            ts=ts,
            body=body,
            loc=loc,
            wire_id=wire_id,
            origin_backend_kind=origin_backend_kind,
            origin_backend_id=origin_backend_id,
            deliveries=deliveries,
            pending_device_ids=pending_device_ids,
            conv_key=conv_key,
            seq=seq,
            group_msg_id=group_msg_id,
            sender_alias=sender_alias,
        )
        if msg is None:
            # §5.2 step 3's dedup: this (wireId, recipient) pair was already
            # delivered by an earlier at-least-once webhook redelivery.
            # Nothing new to fan out.
            return None

        backends_by_id = {b.id: b for b in enabled}
        for bid, delivery in msg.deliveries.items():
            backend_row = backends_by_id.get(bid)
            if backend_row is None:
                continue
            self._deliver_one(msg, bid, delivery, backend_row)
        return msg

    def _deliver_one(
        self, msg: Message, bid: str, delivery: Delivery, backend_row: BackendRow
    ) -> None:
        backend = self._registry.get(backend_row.kind)
        if backend is None:
            logger.warning(
                "no backend implementation registered for kind %r (bid=%s, msg=%s)",
                backend_row.kind,
                bid,
                msg.id,
            )
            return
        try:
            result = backend.deliver(msg, delivery, backend_row)
        except Exception:
            # §5.2: "Any failure leaves the delivery 'queued'/'failed'" --
            # inline delivery is best-effort within the request; a backend
            # bug here must not turn into a 500 for the sender when the
            # message itself was already committed. Counts as a failed
            # attempt below the same as a backend-reported failure, so a
            # `deliver()` that always raises still eventually reaches
            # `failed` rather than retrying forever.
            logger.exception(
                "delivery failed for backend kind=%s bid=%s msg=%s", backend_row.kind, bid, msg.id
            )
            result = DeliverResult(
                ok=False, state=delivery.state, error="deliver() raised (see relay logs)"
            )
        # S2b: apply the outcome `deliver()` itself is not responsible for
        # persisting (docs/SERVER_PLAN.md §5.2's attempts+1/max-5/failed
        # bookkeeping) -- `backends/base.py`'s contract has each adapter own
        # only its *success*-path state transition
        # (`mark_delivery_sent_if_queued`/equivalent); this is the one place
        # that sees every `DeliverResult`, success or failure, so it is the
        # one place that can count attempts and cut off a delivery that can
        # never succeed.
        device_id = delivery.externalId if backend_row.kind == "pager" else None
        messages_store.record_delivery_attempt(
            msg.id, bid, ok=result.ok, error=result.error, device_id=device_id
        )

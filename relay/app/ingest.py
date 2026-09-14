"""Business logic for inbound device traffic, arriving over
`POST /webhooks/mqtt` (see app/routers/webhooks.py) instead of a live MQTT
session. This is what `app/mqtt_gateway.py` did against a `Transport` in the
MVP; the ack state machine, dedup, and the online-edge republish rule are
unchanged (docs/PROTOCOL.md §4/§5) -- only the storage engine underneath
changed (docs/SERVER_PLAN.md §4.8, §5 `ingest.py`).

**Two device-traffic models now live side by side, on purpose** (Phase 3):

- `app/store/legacy.py`'s device-scoped thread (`legacyMessages`,
  `legacyStatus`) -- kept exactly as Phase 2a/2b left it, so the legacy
  `RELAY_TOKEN` endpoints (`app/routers/legacy.py`, deleted in Phase 6) and
  any device id that was never registered via `POST /api/admin/devices`
  keep working unchanged.
- The real uid-addressed model (`docs/SERVER_PLAN.md` §3: `messages/{id}`,
  the allow-list, per-backend `deliveries`), reached through
  `app.routing.Routing`, for any `device_id` that *is* a registered
  `devices/{deviceId}` document (i.e. was created through the admin API).

`_handle_up_message`/`_handle_ack` pick a path by checking whether
`devices/{device_id}` exists -- for an ack this is done implicitly by trying
`messages/{id}` first and falling back to the legacy collection, since a
down message's wire `id` is drawn from the same `m_` id space either way and
there is no cheaper way to tell which store created it than to ask each
once. Both paths honour PROTOCOL.md §4.1's rules identically; only the
document shape underneath differs.

No injected `Store`: `app/store/*` talk to the one process-wide Firestore
client (`app/db/firestore.py`). `Ingest` takes a `BrokerClient` (for the
legacy publish path and the §4.2 `system` reply) and, since it now needs to
fan a v2 up-message out through the allow-list and backend adapters, an
`app.routing.Routing` -- both stay injectable for tests
(`tests/fake_transport.FakeBrokerClient`, a fake/real `Routing`).
"""

from __future__ import annotations

import hashlib
import logging
import time
from typing import Literal

from app import location
from app.broker import BrokerClient
from app.routing import Routing
from app.store import devices as devices_store
from app.store import legacy as legacy_store
from app.store import messages as messages_store
from app.store.legacy import LegacyMessage
from app.wire import (
    SYSTEM_ALIAS,
    LocEnvelope,
    StatusEnvelope,
    UpEnvelope,
    build_down_payload,
    log_malformed,
    parse_envelope_bytes,
    resolve_ts,
)

logger = logging.getLogger("relay.ingest")

# PROTOCOL.md §4.2 case 3: the one exception to "never auto-reply on MQTT".
UNKNOWN_RECIPIENT_BODY = "unknown recipient"


def _device_id_from_topic(topic: str, expected_suffix: str) -> str | None:
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "pager" or parts[2] != expected_suffix:
        return None
    return parts[1]


class Ingest:
    """Owns the relay's reaction to device traffic. Called by the webhook
    router once per inbound event; holds no per-request state itself."""

    def __init__(self, broker: BrokerClient, routing: Routing | None = None) -> None:
        self._broker = broker
        self._routing = routing if routing is not None else Routing(broker)

    # ---- outgoing ----

    def publish_down(self, row: LegacyMessage) -> bool:
        """Build and publish a `/down` envelope for `row` via the broker's
        REST API. Marks the row 'sent' on a 2xx; leaves it 'queued'
        otherwise (a later `/internal/tick` retries it, docs/SERVER_PLAN.md
        §5.8 -- not built in this phase)."""
        payload = build_down_payload(msg_id=row.id, ts=row.ts, body=row.body or "", v=row.v)
        ok = self._broker.publish(f"pager/{row.deviceId}/down", payload, qos=1, retain=False)
        if ok:
            legacy_store.mark_sent(row.id)
        return ok

    # ---- /up ----

    def handle_up(self, topic: str, payload: bytes) -> None:
        device_id = _device_id_from_topic(topic, "up")
        if device_id is None:
            logger.warning("up webhook for unrecognised topic %s dropped", topic)
            return
        data = parse_envelope_bytes(payload)
        if data is None:
            log_malformed(topic, payload, "oversize, non-utf8 or non-JSON-object")
            return
        try:
            env = UpEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            log_malformed(topic, payload, str(exc))
            return

        if env.is_ack:
            self._handle_ack(device_id, env)
        else:
            self._handle_up_message(device_id, env)

    def _handle_ack(self, device_id: str, env: UpEnvelope) -> None:
        assert env.ack is not None and env.ack in ("shown", "read")
        ack_ts = resolve_ts(env.ts)

        # Try the v2 uid-addressed store first -- a down message's wire `id`
        # is the Firestore `messages/{id}` doc id itself (see
        # `app/backends/pager.py`'s module docstring), so this is a cheap,
        # unambiguous existence check, not a guess.
        msg = messages_store.get_message(env.id)
        if msg is not None:
            self._handle_v2_ack(device_id, msg, env.ack, ack_ts)
            return

        row = legacy_store.get_message(env.id)
        if row is None:
            # §4.1 rule 3: unknown id -> log + drop, never create a row.
            logger.info("ack for unknown message id=%s from device=%s dropped", env.id, device_id)
            return
        if row.direction != "down":
            logger.warning("ack references a non-down message id=%s dropped", env.id)
            return
        if row.deviceId != device_id:
            # §4.1 rule 4: wrong device -> drop + log as a security event.
            logger.warning(
                "SECURITY wrong-device ack: pager/%s/up acked message %s addressed to device %s",
                device_id,
                env.id,
                row.deviceId,
            )
            return
        result = legacy_store.apply_ack(env.id, env.ack, ack_ts)
        if result == "noop":
            logger.debug(
                "idempotent ack '%s' for message %s (already %s)", env.ack, env.id, row.state
            )

    def _handle_v2_ack(
        self,
        device_id: str,
        msg: messages_store.Message,
        ack: Literal["shown", "read"],
        ack_ts: int,
    ) -> None:
        bid = messages_store.find_pager_delivery(msg, device_id)
        if bid is None:
            # §4.1 rule 4: this message has no pager delivery addressed to
            # *this* device -- either it was addressed to a different
            # device, or (very unlikely) some other backend's delivery id
            # happens to collide, either way not a message this device may
            # ack.
            logger.warning(
                "SECURITY wrong-device ack: pager/%s/up acked message %s with no matching "
                "pager delivery",
                device_id,
                msg.id,
            )
            return
        result = messages_store.apply_delivery_ack(msg.id, bid, device_id, ack, ack_ts)
        if result == "noop":
            logger.debug("idempotent ack '%s' for message %s backend %s", ack, msg.id, bid)

    def _handle_up_message(self, device_id: str, env: UpEnvelope) -> None:
        device = devices_store.get_device(device_id)
        if device is not None:
            if device.revokedAt is not None:
                # A revoked device is not allowed to inject anything. Falling
                # through to the legacy store here would silently *downgrade*
                # it to the un-allow-listed device-scoped thread instead of
                # dropping it -- same security class as §4.1 rule 4.
                logger.warning(
                    "SECURITY up message %s from revoked device %s dropped", env.id, device_id
                )
                return
            self._handle_v2_up_message(device, env)
            return

        # §4.2: duplicate id (QoS 1 redelivery) -> drop silently. The
        # existence check and the insert are one atomic Firestore
        # transaction (see app.store.legacy.insert_up_message) rather than a
        # separate `id_exists` check followed by an insert, so two
        # concurrent at-least-once webhook deliveries of the same up
        # message can't both race past a pre-check.
        inserted = legacy_store.insert_up_message(
            msg_id=env.id,
            device_id=device_id,
            ts=resolve_ts(env.ts),
            sender=env.from_,
            body=env.body,
        )
        if not inserted:
            logger.debug("duplicate up message id=%s dropped", env.id)

    def _handle_v2_up_message(self, device: devices_store.Device, env: UpEnvelope) -> None:
        # `routing.send()` does the allow-list check, the transactional
        # dedup-by-wireId (covers the same QoS-1/at-least-once redelivery
        # case as the legacy path's `insert_up_message`), and the fan-out to
        # the recipient's enabled backends.
        result = self._routing.send(
            sender_uid=device.ownerUid,
            recipient_alias=env.to,
            kind="text",
            body=env.body,
            origin_backend_kind="pager",
            wire_id=env.id,
            device_id=device.id,
            ts=resolve_ts(env.ts),
        )
        if env.to is not None and result.rejected:
            # PROTOCOL.md §4.2 case 3: an explicit `to` naming an unknown or
            # disallowed recipient -> exactly one `system` down reply, never
            # stored as a message (it is not part of any conversation), rate-
            # limited to one per offending up message.
            self._send_system_reply(device.id, UNKNOWN_RECIPIENT_BODY, cause_id=env.id)

    def _send_system_reply(self, device_id: str, body: str, *, cause_id: str) -> None:
        # The reply's wire `id` is *derived from the offending up message's
        # id*, not freshly minted: §4.2 rate-limits this reply to one per
        # offending up message, and a QoS 1 redelivery of that up message is
        # the same offending message. A fresh id each time would re-render
        # and re-alert on the device (§4.1 rule 7's dedup ring keys on `id`),
        # turning one bad `to` into an alert per redelivery. Same shape as
        # `new_id("m_")` (`m_` + 8 hex), so §3.1/§3.3 are unaffected.
        msg_id = "m_" + hashlib.sha256(f"sys:{device_id}:{cause_id}".encode()).hexdigest()[:8]
        payload = build_down_payload(
            msg_id=msg_id, ts=int(time.time()), body=body, from_=SYSTEM_ALIAS
        )
        self._broker.publish(f"pager/{device_id}/down", payload, qos=1, retain=False)

    # ---- /status ----

    def handle_status(self, topic: str, payload: bytes) -> None:
        device_id = _device_id_from_topic(topic, "status")
        if device_id is None:
            logger.warning("status webhook for unrecognised topic %s dropped", topic)
            return
        data = parse_envelope_bytes(payload)
        if data is None:
            log_malformed(topic, payload, "oversize, non-utf8 or non-JSON-object")
            return
        try:
            env = StatusEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            log_malformed(topic, payload, str(exc))
            return

        previous = legacy_store.get_status(device_id)
        resolved_ts = resolve_ts(env.ts) if env.ts is not None else None
        legacy_store.upsert_status(
            device_id,
            state=env.state,
            mode=env.mode,
            batt_mv=env.batt_mv,
            rssi=env.rssi,
            session=env.session,
            ts=resolved_ts,
            fw=env.fw,
        )

        device = devices_store.get_device(device_id)
        if device is not None:
            # docs/SERVER_PLAN.md §3's real `devices/{deviceId}.status` --
            # kept in lockstep with `legacyStatus` (same envelope, same
            # webhook call) rather than migrated wholesale, since the legacy
            # collection is still what `app/routers/legacy.py`'s GET
            # endpoint reads until Phase 6 deletes it.
            devices_store.update_status(
                device_id,
                state=env.state,
                mode=env.mode,
                battMv=env.batt_mv,
                rssi=env.rssi,
                session=env.session,
                ts=resolved_ts,
                fw=env.fw,
                locPeriodS=env.loc_period_s,
                locMinS=env.loc_min_s,
            )

        if env.state != "online":
            # §5.3: bare offline (retained or LWT) -> mark offline, no
            # message state changes, no republish.
            return

        session_changed = previous is None or previous.session != env.session
        offline_to_online = previous is not None and previous.state == "offline"
        if session_changed or offline_to_online:
            self._republish_unacked(device_id)
            if device is not None:
                self._republish_unacked_v2(device_id)

    def retry_queued(self, device_id: str) -> None:
        """Opportunistically retry **legacy** down messages still 'queued'
        (their broker publish never got a 2xx) for `device_id`. This is a
        lazy-at-read-time stand-in, scoped to the legacy device-scoped
        thread only, for the one gap the online-edge republish leaves: a
        *transient* publish failure (timeout, 5xx, DNS blip) while the
        device stays continuously connected. Called from the legacy GET
        endpoint (app/routers/legacy.py) -- see docs/SERVER_PLAN.md §2
        decision 7, "derived at read time".

        **Not superseded by `app.jobs.tick()`** (docs/SERVER_PLAN.md §5.8),
        despite doing the same *kind* of thing: `tick()` retries the v2
        uid-addressed model's `pendingDeviceIds`/`messages/{id}` deliveries,
        which the legacy per-device thread has no equivalent of (see
        `app/store/legacy.py`'s module docstring for why that thread is a
        separate, intentionally non-migrated collection). The two mechanisms
        stay side by side until Phase 6 deletes the legacy endpoints and this
        method along with them."""
        for row in legacy_store.get_queued_down_messages(device_id):
            if not self.publish_down(row):
                # The broker is unreachable (the only reason a publish that
                # once failed fails again the same way), so stop: this runs
                # on a *read*, and paying PUBLISH_TIMEOUT_S per remaining
                # message would turn a routine GET into a multi-tens-of-
                # seconds request exactly while the broker is down.
                logger.info(
                    "retry_queued for device %s stopped early: broker publish still failing",
                    device_id,
                )
                return

    def _republish_unacked(self, device_id: str) -> None:
        candidates = legacy_store.get_republish_candidates(device_id)
        for row in candidates:
            logger.info("re-publishing unacked message %s to device %s", row.id, device_id)
            self.publish_down(row)

    def _republish_unacked_v2(self, device_id: str) -> None:
        """The v2 counterpart of `_republish_unacked`: PROTOCOL.md §5.3's
        online-edge re-publish, sourced from `pendingDeviceIds` (oldest
        first, capped at 10 -- both enforced by
        `messages_store.list_pending_for_device`) instead of the legacy
        per-device thread.

        §5.3's selection rule also *excludes* `kind:"loc_req"` ("a location
        request that missed its window is worthless"). No `loc_req` message
        exists before Phase 4, but the filter belongs with the rule it
        implements, not with the phase that first makes it observable."""
        for msg in messages_store.list_pending_for_device(device_id):
            if msg.kind == "loc_req":
                continue
            logger.info("re-publishing unacked v2 message %s to device %s", msg.id, device_id)
            self._routing.redeliver_pager(msg, device_id)

    # ---- /loc ----

    def handle_loc(self, topic: str, payload: bytes) -> None:
        """Validate per docs/PROTOCOL.md §13.2, then hand off to
        `app.location.ingest_loc` for the real handling (dedup,
        `devices/{d}/locations`, `loc_req` fulfilment -- SERVER_PLAN.md
        §5.6). No `Routing` needed here: unlike `_handle_v2_up_message`,
        `ingest_loc` never originates a new delivery -- it only resolves an
        *existing* `loc_req`'s delivery to `fulfilled`, which is a plain
        Firestore transaction, not a broker publish."""
        device_id = _device_id_from_topic(topic, "loc")
        if device_id is None:
            logger.warning("loc webhook for unrecognised topic %s dropped", topic)
            return
        data = parse_envelope_bytes(payload)
        if data is None:
            log_malformed(topic, payload, "oversize, non-utf8 or non-JSON-object")
            return
        try:
            env = LocEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            log_malformed(topic, payload, str(exc))
            return
        location.ingest_loc(device_id, env)

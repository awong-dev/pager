"""Business logic for inbound device traffic, arriving over
`POST /webhooks/mqtt` (see app/routers/webhooks.py) instead of a live MQTT
session. This is what `app/mqtt_gateway.py` did against a `Transport` in the
MVP; the ack state machine, dedup, and the online-edge republish rule are
unchanged (docs/PROTOCOL.md §4/§5) -- only the storage engine underneath
changed (docs/SERVER_PLAN.md §4.8, §5 `ingest.py`): `app/store/legacy.py`'s
Firestore collections instead of `app/store.py`'s SQLite tables. Full
uid-addressed routing (`messages/{id}`, allow-list, fan-out) is Phase 3;
until then, ingest keeps the MVP's device-scoped thread and status shape so
the legacy endpoints (`app/routers/legacy.py`) keep working unchanged.

No injected `Store`: `app/store/legacy.py`'s functions talk to the one
process-wide Firestore client (`app/db/firestore.py`), the same pattern
every other `app/store/*` module uses. `Ingest` only takes a `BrokerClient`,
which stays injectable for tests (`tests/fake_transport.FakeBrokerClient`).
"""

from __future__ import annotations

import logging

from app.broker import BrokerClient
from app.store import legacy as legacy_store
from app.store.legacy import LegacyMessage
from app.wire import (
    LocEnvelope,
    StatusEnvelope,
    UpEnvelope,
    build_down_payload,
    log_malformed,
    parse_envelope_bytes,
    resolve_ts,
)

logger = logging.getLogger("relay.ingest")


def _device_id_from_topic(topic: str, expected_suffix: str) -> str | None:
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "pager" or parts[2] != expected_suffix:
        return None
    return parts[1]


class Ingest:
    """Owns the relay's reaction to device traffic. Called by the webhook
    router once per inbound event; holds no per-request state itself."""

    def __init__(self, broker: BrokerClient) -> None:
        self._broker = broker

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
        assert env.ack is not None
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
        ack_ts = resolve_ts(env.ts)
        result = legacy_store.apply_ack(env.id, env.ack, ack_ts)
        if result == "noop":
            logger.debug(
                "idempotent ack '%s' for message %s (already %s)", env.ack, env.id, row.state
            )

    def _handle_up_message(self, device_id: str, env: UpEnvelope) -> None:
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

        if env.state != "online":
            # §5.3: bare offline (retained or LWT) -> mark offline, no
            # message state changes, no republish.
            return

        session_changed = previous is None or previous.session != env.session
        offline_to_online = previous is not None and previous.state == "offline"
        if session_changed or offline_to_online:
            self._republish_unacked(device_id)

    # TODO(orchestrator): remove when /internal/tick lands (SERVER_PLAN.md §5.8)
    def retry_queued(self, device_id: str) -> None:
        """Opportunistically retry down messages still 'queued' (their
        broker publish never got a 2xx) for `device_id`. This is a
        lazy-at-read-time stand-in for docs/SERVER_PLAN.md §5.8's
        `/internal/tick` (not built in this phase) for the one gap it would
        otherwise leave: a *transient* publish failure (timeout, 5xx, DNS
        blip) while the device stays continuously connected. A full broker
        outage does not need this -- the device would reconnect once the
        broker recovers, producing a fresh session and an `online` `/status`
        webhook, which already triggers a republish via `session_changed` in
        `handle_status`. It's specifically the case where `publish_down`
        fails but the device's own MQTT session never drops that neither of
        `_republish_unacked`'s two trigger conditions can ever catch.
        Called from the legacy GET endpoint (app/routers/legacy.py) -- see
        `docs/SERVER_PLAN.md` §2 decision 7, "derived at read time"."""
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

    # ---- /loc ----

    def handle_loc(self, topic: str, payload: bytes) -> None:
        """Stub: validate and log per docs/PROTOCOL.md §13.2. Real handling
        (dedup, `devices/{d}/locations`, `loc_req` fulfilment) is
        docs/SERVER_PLAN.md §5.6, Phase 4 -- this only exists so `/loc`
        webhooks don't 404 or crash in the meantime."""
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
        logger.info(
            "loc from device=%s id=%s req=%s cached=%s err=%s (stub, no-op until Phase 4)",
            device_id,
            env.id,
            env.req,
            env.cached,
            env.err,
        )

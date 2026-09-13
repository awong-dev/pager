"""MQTT business logic for the relay: ack state machine, up-message storage,
status/LWT handling and the online-edge republish rule.

This is deliberately transport-agnostic (see the `Transport` protocol below)
so it can run unchanged against either `app.mqtt_transport.PahoTransport`
(production) or an in-process fake (tests). Nothing in this module opens a
socket.
"""

from __future__ import annotations

import logging
import threading
from typing import Protocol

from app.store import MessageRow, Store
from app.wire import (
    StatusEnvelope,
    UpEnvelope,
    build_down_payload,
    log_malformed,
    parse_envelope_bytes,
    resolve_ts,
)

logger = logging.getLogger("relay.gateway")

UP_WILDCARD = "pager/+/up"
STATUS_WILDCARD = "pager/+/status"


class Transport(Protocol):
    """The subset of a paho-mqtt-like client that MqttGateway needs."""

    def set_handlers(
        self,
        *,
        on_connect,
        on_disconnect,
        on_message,
        on_publish,
    ) -> None: ...

    def connect(self) -> None: ...
    def start(self) -> None: ...
    def stop(self) -> None: ...
    def subscribe(self, topic: str, qos: int) -> None: ...
    def publish(self, topic: str, payload: bytes, qos: int, retain: bool) -> int: ...


def _split_topic(topic: str) -> tuple[str, str] | None:
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "pager":
        return None
    device_id, kind = parts[1], parts[2]
    if kind not in ("up", "status"):
        return None
    return device_id, kind


class MqttGateway:
    """Owns the relay's MQTT session and applies docs/PROTOCOL.md §4/§5."""

    def __init__(self, store: Store, transport: Transport) -> None:
        self._store = store
        self._transport = transport
        self._mid_to_id: dict[int, str] = {}
        self._lock = threading.Lock()
        transport.set_handlers(
            on_connect=self._on_connect,
            on_disconnect=self._on_disconnect,
            on_message=self._on_message,
            on_publish=self._on_publish,
        )

    def start(self) -> None:
        self._transport.connect()
        self._transport.start()

    def stop(self) -> None:
        self._transport.stop()

    # ---- outgoing ----

    def publish_down(self, row: MessageRow) -> None:
        payload = build_down_payload(msg_id=row.id, ts=row.ts, body=row.body or "", v=row.v)
        mid = self._transport.publish(
            f"pager/{row.device_id}/down", payload, qos=1, retain=False
        )
        with self._lock:
            self._mid_to_id[mid] = row.id

    # ---- transport callbacks ----

    def _on_connect(self, session_present: bool) -> None:
        logger.info("connected to broker (session_present=%s)", session_present)
        # Always (re)subscribe: cheap and idempotent, and covers both the
        # first connect and any reconnect after session loss.
        self._transport.subscribe(UP_WILDCARD, qos=1)
        self._transport.subscribe(STATUS_WILDCARD, qos=1)

    def _on_disconnect(self, rc: int) -> None:
        logger.warning("disconnected from broker (rc=%s)", rc)

    def _on_publish(self, mid: int) -> None:
        with self._lock:
            msg_id = self._mid_to_id.pop(mid, None)
        if msg_id is not None:
            self._store.mark_sent(msg_id)

    def _on_message(self, topic: str, payload: bytes) -> None:
        # §3.4: never crash the MQTT loop on a parse error.
        try:
            split = _split_topic(topic)
            if split is None:
                logger.warning("message on unrecognised topic %s dropped", topic)
                return
            device_id, kind = split
            data = parse_envelope_bytes(payload)
            if data is None:
                log_malformed(topic, payload, "oversize, non-utf8 or non-JSON-object")
                return
            if kind == "up":
                self._handle_up(device_id, data, topic, payload)
            else:
                self._handle_status(device_id, data, topic, payload)
        except Exception:
            logger.exception("unexpected error handling message on %s", topic)

    # ---- /up ----

    def _handle_up(self, device_id: str, data: dict, topic: str, raw: bytes) -> None:
        try:
            env = UpEnvelope.model_validate(data)
        except Exception as exc:  # pydantic.ValidationError, narrowly caught above
            log_malformed(topic, raw, str(exc))
            return

        if env.is_ack:
            self._handle_ack(device_id, env)
        else:
            self._handle_up_message(device_id, env)

    def _handle_ack(self, device_id: str, env: UpEnvelope) -> None:
        assert env.ack is not None
        row = self._store.get_message(env.id)
        if row is None:
            # §4.1 rule 3: unknown id -> log + drop, never create a row.
            logger.info("ack for unknown message id=%s from device=%s dropped", env.id, device_id)
            return
        if row.direction != "down":
            logger.warning("ack references a non-down message id=%s dropped", env.id)
            return
        if row.device_id != device_id:
            # §4.1 rule 4: wrong device -> drop + log as a security event.
            logger.warning(
                "SECURITY wrong-device ack: pager/%s/up acked message %s addressed to device %s",
                device_id,
                env.id,
                row.device_id,
            )
            return
        ack_ts = resolve_ts(env.ts)
        result = self._store.apply_ack(env.id, env.ack, ack_ts)
        if result == "noop":
            logger.debug(
                "idempotent ack '%s' for message %s (already %s)", env.ack, env.id, row.state
            )

    def _handle_up_message(self, device_id: str, env: UpEnvelope) -> None:
        if self._store.id_exists(env.id):
            # §4.2: duplicate id (QoS 1 redelivery) -> drop silently.
            logger.debug("duplicate up message id=%s dropped", env.id)
            return
        self._store.insert_up_message(
            msg_id=env.id,
            device_id=device_id,
            ts=resolve_ts(env.ts),
            sender=env.from_,
            body=env.body,
        )

    # ---- /status ----

    def _handle_status(self, device_id: str, data: dict, topic: str, raw: bytes) -> None:
        try:
            env = StatusEnvelope.model_validate(data)
        except Exception as exc:
            log_malformed(topic, raw, str(exc))
            return

        previous = self._store.get_status(device_id)
        resolved_ts = resolve_ts(env.ts) if env.ts is not None else None
        self._store.upsert_status(
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

    def _republish_unacked(self, device_id: str) -> None:
        candidates = self._store.get_republish_candidates(device_id)
        for row in candidates:
            logger.info("re-publishing unacked message %s to device %s", row.id, device_id)
            self.publish_down(row)

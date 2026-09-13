"""paho-mqtt-backed implementation of the `Transport` protocol used by
MqttGateway. All paho-specific details (callback API version, reconnect
backoff, client id / clean-session per PROTOCOL.md §6.1) live here so
app/mqtt_gateway.py stays testable without a socket.
"""

from __future__ import annotations

import logging
from typing import Callable

import paho.mqtt.client as mqtt

logger = logging.getLogger("relay.mqtt.transport")

# §6.1: backoff 5s, 15s, 60s, 300s, then 300s steady. paho's own
# reconnect_delay_set does exponential doubling from min_delay up to
# max_delay rather than this exact schedule; 5s/300s bounds are the closest
# a stock paho reconnect loop gets to the documented schedule without
# reimplementing paho's internal reconnect thread.
RECONNECT_MIN_DELAY_S = 5
RECONNECT_MAX_DELAY_S = 300

# The device's 1800s keepalive (§6.2) is sized for its battery/eDRX budget.
# The relay has no such constraint, so it uses a normal always-connected
# service keepalive instead.
RELAY_KEEPALIVE_S = 60


def _reason_code_to_int(reason_code: object) -> int:
    """Normalize a paho v2 callback's reason/return code to a plain int.

    paho-mqtt 2.x's VERSION2 callback API passes a `paho.mqtt.reasoncodes.
    ReasonCode` instance (not a plain int) for connect/disconnect reason
    codes. `ReasonCode` has no `__int__`, so `int(reason_code)` raises
    `TypeError`; since that happens inside paho's own callback dispatch
    (the `loop_start()` background thread), an uncaught exception there
    kills the thread and the relay never reconnects again. `ReasonCode.value`
    holds the actual numeric code. Plain ints (e.g. from tests or future
    paho versions) and `None` are also accepted defensively.
    """
    if reason_code is None:
        return 0
    value = getattr(reason_code, "value", reason_code)
    return int(value)


class PahoTransport:
    def __init__(
        self,
        *,
        client_id: str,
        host: str,
        port: int,
        username: str | None = None,
        password: str | None = None,
        keepalive: int = RELAY_KEEPALIVE_S,
    ) -> None:
        self._host = host
        self._port = port
        self._keepalive = keepalive

        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
            # §6.1: clean_session=False + a stable client id ("relay-1") so
            # the broker queues QoS 1 /up, /status while the relay is briefly
            # disconnected.
            clean_session=False,
            protocol=mqtt.MQTTv311,
        )
        if username:
            self._client.username_pw_set(username, password)
        self._client.reconnect_delay_set(
            min_delay=RECONNECT_MIN_DELAY_S, max_delay=RECONNECT_MAX_DELAY_S
        )

        self._on_connect_cb: Callable[[bool], None] | None = None
        self._on_disconnect_cb: Callable[[int], None] | None = None
        self._on_message_cb: Callable[[str, bytes], None] | None = None
        self._on_publish_cb: Callable[[int], None] | None = None

        self._client.on_connect = self._handle_connect
        self._client.on_disconnect = self._handle_disconnect
        self._client.on_message = self._handle_message
        self._client.on_publish = self._handle_publish

    def set_handlers(self, *, on_connect, on_disconnect, on_message, on_publish) -> None:
        self._on_connect_cb = on_connect
        self._on_disconnect_cb = on_disconnect
        self._on_message_cb = on_message
        self._on_publish_cb = on_publish

    def connect(self) -> None:
        self._client.connect(self._host, self._port, keepalive=self._keepalive)

    def start(self) -> None:
        self._client.loop_start()

    def stop(self) -> None:
        self._client.loop_stop()
        try:
            self._client.disconnect()
        except Exception:
            logger.debug("disconnect during stop() raised, ignoring", exc_info=True)

    def subscribe(self, topic: str, qos: int) -> None:
        self._client.subscribe(topic, qos=qos)

    def publish(self, topic: str, payload: bytes, qos: int, retain: bool) -> int:
        info = self._client.publish(topic, payload, qos=qos, retain=retain)
        return info.mid

    # ---- paho v2 callback signatures ----

    def _handle_connect(self, client, userdata, connect_flags, reason_code, properties) -> None:
        session_present = bool(getattr(connect_flags, "session_present", False))
        if self._on_connect_cb:
            self._on_connect_cb(session_present)

    def _handle_disconnect(
        self, client, userdata, disconnect_flags, reason_code, properties
    ) -> None:
        # paho 2.x's v2 callback API passes a `ReasonCode` object here, not a
        # plain int -- `ReasonCode` has no `__int__`, so `int(reason_code)`
        # raises TypeError. That's uncaught inside paho's own callback
        # dispatch (loop_start()'s thread), which silently kills the
        # background MQTT thread and the relay never reconnects again.
        # `.value` is the actual numeric reason code on both `ReasonCode` and
        # plain ints (via getattr fallback below).
        if self._on_disconnect_cb:
            self._on_disconnect_cb(_reason_code_to_int(reason_code))

    def _handle_message(self, client, userdata, msg) -> None:
        if self._on_message_cb:
            self._on_message_cb(msg.topic, msg.payload)

    def _handle_publish(self, client, userdata, mid, reason_code=None, properties=None) -> None:
        if self._on_publish_cb:
            self._on_publish_cb(mid)

"""An in-process fake standing in for a paho-mqtt client + broker.

Implements exactly the `app.mqtt_gateway.Transport` protocol, plus a few
test-only helpers (`ack`, `deliver`, `simulate_reconnect`, ...) to drive the
gateway's callbacks deterministically without a real socket or broker
process. `publish()` never auto-acks -- tests call `ack()`/`ack_last()`
explicitly to simulate the broker's PUBACK, so "state becomes sent on
PUBACK" is an observable, controllable step rather than something that just
always happens.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class PublishedMessage:
    topic: str
    payload: bytes
    qos: int
    retain: bool
    mid: int


class FakeTransport:
    def __init__(self) -> None:
        self.subscriptions: list[tuple[str, int]] = []
        self.published: list[PublishedMessage] = []
        self.connected = False
        self._next_mid = 1

        self._on_connect = None
        self._on_disconnect = None
        self._on_message = None
        self._on_publish = None

    # ---- Transport protocol ----

    def set_handlers(self, *, on_connect, on_disconnect, on_message, on_publish) -> None:
        self._on_connect = on_connect
        self._on_disconnect = on_disconnect
        self._on_message = on_message
        self._on_publish = on_publish

    def connect(self) -> None:
        self.connected = True

    def start(self) -> None:
        # Simulate the broker's CONNACK arriving right after connect().
        self.simulate_reconnect(session_present=False)

    def stop(self) -> None:
        self.connected = False

    def subscribe(self, topic: str, qos: int) -> None:
        self.subscriptions.append((topic, qos))

    def publish(self, topic: str, payload: bytes, qos: int, retain: bool) -> int:
        mid = self._next_mid
        self._next_mid += 1
        self.published.append(PublishedMessage(topic, payload, qos, retain, mid))
        return mid

    # ---- test helpers ----

    def ack(self, mid: int) -> None:
        """Simulate the broker returning PUBACK for `mid`."""
        if self._on_publish:
            self._on_publish(mid)

    def ack_last(self) -> None:
        self.ack(self.published[-1].mid)

    def ack_all_pending(self) -> None:
        for msg in self.published:
            self.ack(msg.mid)

    def deliver(self, topic: str, payload: bytes) -> None:
        """Simulate an incoming PUBLISH from the broker (device -> relay)."""
        if self._on_message:
            self._on_message(topic, payload)

    def simulate_disconnect(self, rc: int = 1) -> None:
        self.connected = False
        if self._on_disconnect:
            self._on_disconnect(rc)

    def simulate_reconnect(self, session_present: bool = False) -> None:
        self.connected = True
        if self._on_connect:
            self._on_connect(session_present)

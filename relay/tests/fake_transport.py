"""A fake `app.broker.BrokerClient` for tests.

Records every attempted publish in `published` and never touches a socket.
Unit tests either call `app.ingest.Ingest.handle_up/handle_status/handle_loc`
directly with webhook-shaped payloads (see the `up_topic`/`status_topic`
helpers in tests/conftest.py), or POST through the FastAPI test client's
`/webhooks/mqtt` using `webhook_body()`/`WEBHOOK_KEY` for the real dispatch
path -- either way, there is no real broker underneath.

(Kept as `tests/fake_transport.py`, not renamed, even though it no longer
implements `app.mqtt_gateway.Transport` -- that module is gone; this is now
the one fake test double for the broker.)
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from app.broker import BrokerClient

WEBHOOK_KEY = "test-webhook-key"


@dataclass
class PublishedMessage:
    topic: str
    payload: bytes
    qos: int
    retain: bool


class FakeBrokerClient:
    """Drop-in stand-in for `app.broker.BrokerClient`. `publish()` succeeds
    and records the call unless `fail_publish` is set (simulates the
    broker's REST API being unreachable -- e.g. an outage, where the caller
    must leave the message 'queued' rather than raise)."""

    def __init__(self, *, webhook_key: str = WEBHOOK_KEY) -> None:
        self.published: list[PublishedMessage] = []
        self.fail_publish = False
        self._webhook_key = webhook_key

    def publish(self, topic: str, payload: bytes, qos: int, retain: bool) -> bool:
        if self.fail_publish:
            return False
        self.published.append(PublishedMessage(topic, payload, qos, retain))
        return True

    def verify_webhook(self, request: Any) -> bool:
        # Exercises the exact same header/constant-time-compare contract as
        # the real BrokerClient, just against this fake's own configured key
        # rather than Settings.webhook_key.
        import hmac

        provided = request.headers.get("X-Relay-Webhook-Key", "")
        return hmac.compare_digest(provided, self._webhook_key)

    @staticmethod
    def parse_webhook(body: bytes) -> tuple[str, bytes, int] | None:
        # Real parsing logic (no broker-specific behaviour to fake) -- reuse
        # it directly rather than duplicating it.
        return BrokerClient.parse_webhook(body)

    # ---- test helpers ----

    def clear(self) -> None:
        self.published.clear()


def webhook_event(topic: str, payload: bytes, qos: int = 1) -> dict:
    """The JSON shape `tools/emqx_setup.py` configures EMQX's HTTP action to
    POST (see app/broker.py's module docstring): `payload` is a JSON string,
    not raw bytes/base64."""
    return {"topic": topic, "payload": payload.decode("utf-8"), "qos": qos}


def webhook_body(topic: str, payload: bytes, qos: int = 1) -> bytes:
    return json.dumps(webhook_event(topic, payload, qos)).encode("utf-8")


def webhook_headers(webhook_key: str = WEBHOOK_KEY) -> dict[str, str]:
    return {"X-Relay-Webhook-Key": webhook_key}

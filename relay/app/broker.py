"""HTTP client for the MQTT broker's REST publish API, plus the webhook
helpers used by app/routers/webhooks.py.

Per docs/SERVER_PLAN.md §2 decision 1 / §5 (`broker.py`): the relay never
holds an MQTT connection. Outbound `/down` publishes go through the
broker's REST API (`publish`); inbound device traffic arrives over HTTPS on
`POST /webhooks/mqtt`, authenticated by a shared-secret header
(`verify_webhook`) and shaped by whatever the broker's rule engine's HTTP
action actually POSTs (`parse_webhook`).

**Webhook body shape this module expects** (and what `tools/emqx_setup.py`
configures EMQX's HTTP action to send, via its `body: "${.}"` template,
which serializes the whole rule-engine event context as JSON): a JSON
object containing at least

    {"topic": "pager/<id>/up", "payload": "<the device's raw JSON envelope, as a string>", "qos": 1, ...other fields ignored...}

`payload` is a JSON *string* (EMQX does not base64-encode a text MQTT
payload by default), so `parse_webhook` re-encodes it to UTF-8 bytes to
match `wire.py`'s `parse_envelope_bytes(bytes) -> ...` contract. A payload
that is already bytes/str-of-base64 is not produced by this rule
configuration and is out of scope.
"""

from __future__ import annotations

import hmac
import json
import logging
from typing import Any

import httpx

from app.config import Settings

logger = logging.getLogger("relay.broker")

PUBLISH_TIMEOUT_S = 5.0
HEALTHCHECK_TIMEOUT_S = 3.0
WEBHOOK_KEY_HEADER = "X-Relay-Webhook-Key"


class BrokerClient:
    """REST-only view of the broker: publish, plus webhook auth/parsing.

    Deliberately holds no socket and no background state -- every method is
    a single, synchronous HTTP call (or none at all), which is what makes
    this safe to construct fresh per request/per app instance."""

    def __init__(self, settings: Settings) -> None:
        self._base_url = settings.broker_api_url.rstrip("/")
        self._api_key = settings.broker_api_key
        self._api_secret = settings.broker_api_secret
        self._webhook_key = settings.webhook_key

    def publish(self, topic: str, payload: bytes, qos: int, retain: bool) -> bool:
        """POST to the broker's REST publish API (EMQX's `/publish`).
        Returns True on a 2xx response, False on anything else -- including
        a network-level failure -- and never raises. A failed publish is a
        delivery-layer concern (the caller leaves the message 'queued';
        retrying it is `/internal/tick`'s job, docs/SERVER_PLAN.md §5.8, not
        this method's)."""
        auth = (self._api_key, self._api_secret or "") if self._api_key else None
        body = {
            "topic": topic,
            "payload": payload.decode("utf-8"),
            "qos": qos,
            "retain": retain,
            "payload_encoding": "plain",
        }
        try:
            resp = httpx.post(
                f"{self._base_url}/publish", json=body, auth=auth, timeout=PUBLISH_TIMEOUT_S
            )
        except httpx.HTTPError:
            logger.warning("broker publish request failed (topic=%s)", topic, exc_info=True)
            return False
        if 200 <= resp.status_code < 300:
            return True
        logger.warning(
            "broker publish rejected (topic=%s status=%s body=%r)",
            topic,
            resp.status_code,
            resp.text[:200],
        )
        return False

    def healthcheck(self) -> bool:
        """`(build addition, phase 8 hardening)`: `GET {base}/status` --
        EMQX's plain-text, unauthenticated liveness endpoint (the same one
        `tools/emqx_setup.py` polls while waiting for the broker container
        to come up, `GET {base_url}/api/v5/status`; `self._base_url` already
        ends in `/api/v5`, per `Settings.broker_api_url`'s own default and
        docstring). `GET /healthz` (docs/SERVER_PLAN.md §5.1: "200 +
        firestore reachable + broker API reachable") uses this -- a network
        failure or a non-2xx both mean "broker unreachable", never an
        exception the caller has to handle, matching `publish()`'s own
        never-raises contract."""
        try:
            resp = httpx.get(f"{self._base_url}/status", timeout=HEALTHCHECK_TIMEOUT_S)
        except httpx.HTTPError:
            return False
        return 200 <= resp.status_code < 300

    def verify_webhook(self, request: Any) -> bool:
        """Constant-time check of the shared-secret header. `request` is a
        `starlette.requests.Request` (typed `Any` to keep this module free
        of a FastAPI import for the pure-Python unit tests)."""
        provided = request.headers.get(WEBHOOK_KEY_HEADER, "")
        expected = self._webhook_key or ""
        # A blank configured key must never match a blank/missing header --
        # fail closed rather than treat "unconfigured" as "open".
        if not expected:
            return False
        return hmac.compare_digest(provided, expected)

    @staticmethod
    def parse_webhook(body: bytes) -> tuple[str, bytes, int] | None:
        """Parse the broker rule-engine HTTP action's POST body (see the
        module docstring for the exact shape). Returns `(topic, payload,
        qos)` or None if the body doesn't match that shape at all -- the
        caller (routers/webhooks.py) still replies 2xx either way per
        PROTOCOL.md §3.4 (a webhook body the relay can't even parse must not
        make the broker retry-storm it), it just can't dispatch anywhere."""
        try:
            data = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None
        if not isinstance(data, dict):
            return None
        topic = data.get("topic")
        payload = data.get("payload")
        qos = data.get("qos", 0)
        if not isinstance(topic, str) or payload is None:
            return None
        if isinstance(payload, str):
            payload_bytes = payload.encode("utf-8")
        elif isinstance(payload, (bytes, bytearray)):
            payload_bytes = bytes(payload)
        else:
            return None
        if not isinstance(qos, int) or isinstance(qos, bool):
            return None
        return topic, payload_bytes, qos

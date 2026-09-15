"""HTTP client for the MQTT broker's REST publish API, plus the webhook
helpers used by app/routers/webhooks.py.

Per docs/SERVER_PLAN.md §2 decision 1 / §5 (`broker.py`): the relay never
holds an MQTT connection. Outbound `/down` publishes go through the
broker's REST API (`publish`); inbound device traffic arrives over HTTPS on
`POST /webhooks/mqtt`, authenticated by a shared-secret header
(`verify_webhook`) and shaped by whatever the broker's rule engine's HTTP
action actually POSTs (`parse_webhook`).

**`publish_down` is the one path to `/down`** (docs/DEVICE_TASKS.md S1.4,
`docs/PROTOCOL.md` §14.5): every caller that wants to publish to a device
(`app/backends/pager.py`'s `deliver()`, and through it both the ordinary
inline send and the online-edge/`/internal/tick` redelivery in
`app/routing.py`) hands `publish_down` the down envelope's fields as a plain
dict -- `{v, id, ts, kind?, from, body?, ack}`, the same shape
`wire.DownEnvelope` models -- and `publish_down` does the rest: reads
`devices/{d}.wire` (which encoding to answer in) and `.authMode` (whether to
sign at all), and for an `authMode: "hmac"` device takes a fresh
`deviceSecrets/{d}.downN` (`app/store/device_secrets.py`'s `next_down_n`,
itself a transaction, per §14.5's "atomic and never repeats") and signs with
`app/devauth.py`'s `sign_cbor`/`sign_json`. No other module builds a signed
(or unsigned) `/down` payload -- `PagerBackend.deliver()` is the only
caller today, so "no other code path may publish to `/down`" is enforced by
there being nowhere else in the codebase that reaches for `hmacKey` or
`downN`.

A device with `authMode: "hmac"` but no `deviceSecrets/{d}` row (a state
that should not exist once S2.2's provisioning flow lands, but does today --
`POST /api/admin/devices` does not yet create one) fails closed: logged and
dropped rather than silently sent unsigned, mirroring `app/ingest.py`'s own
inbound rule (`_verify_and_decode`: no secret => `ok=False`, never "treat as
unsigned"). The delivery stays `queued` for `/internal/tick` to retry, same
as any other publish failure.

`devices/{d}.wire` is `None` until the device's first accepted inbound
envelope (nothing to derive the answering encoding from yet); `publish_down`
defaults that case to `"json"`, `DEVICE_PLAN.md` §2.4's own default for
"logs and humans" when there is no device-observed preference yet.

**Binary payloads are the normal case, in both directions.** A
`docs/PROTOCOL.md` §3.1 CBOR envelope, and every HMAC signature inside one
(§14), is arbitrary bytes; so is S2b.1's AES-GCM bootstrap blob. Neither
direction may assume the payload is UTF-8 text:

* outbound, `publish` base64-encodes the payload and sends EMQX's
  `payload_encoding: "base64"` (`POST /api/v5/publish`), which EMQX decodes
  back to the exact bytes before publishing them. The device sees the same
  MQTT message either way -- this is purely how the relay hands bytes to the
  broker over REST.
* inbound, the webhook body carries the payload base64-encoded in its own
  field (see below), because the raw one cannot survive JSON.

**Webhook body shape this module expects** (and what `tools/emqx_setup.py`
configures EMQX's HTTP action to send, via its `body: "${.}"` template,
which serializes the whole rule-engine event context as JSON): a JSON
object containing at least

    {"topic": "pager/<id>/up",
     "payload_b64": "<the device's raw envelope, base64>",
     "payload": "<the same bytes, lossy>",
     "qos": 1, ...other fields ignored...}

`payload_b64` comes from that rule's `base64_encode(payload)` and is the
field `parse_webhook` uses: EMQX's JSON encoder replaces every invalid
UTF-8 byte of `payload` with U+FFFD (verified against EMQX 5.8.0), so
`payload` is unrecoverable for anything but pure-text envelopes. It is
still accepted as a fallback -- UTF-8-encoded, as before -- so a relay
running against a broker provisioned by an older `emqx_setup.py` keeps
handling JSON-wire devices instead of dropping them wholesale.
"""

from __future__ import annotations

import base64
import binascii
import hmac
import json
import logging
from typing import Any

import httpx

from app import devauth, wirecbor
from app.config import Settings
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store

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
        this method's).

        Always `payload_encoding: "base64"` (see the module docstring), not
        "base64 only when the bytes aren't valid UTF-8": a conditional here
        would make the binary path the rarely-exercised one, which is
        exactly how `payload.decode("utf-8")` survived long enough to 500
        every `POST /api/admin/devices` once S2b.1's encrypted bootstrap
        blob started flowing through it. One code path, same bytes on the
        wire."""
        auth = (self._api_key, self._api_secret or "") if self._api_key else None
        body = {
            "topic": topic,
            "payload": base64.b64encode(payload).decode("ascii"),
            "qos": qos,
            "retain": retain,
            "payload_encoding": "base64",
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

    def publish_down(self, device_id: str, obj: dict[str, Any]) -> bool:
        """The one path to `pager/{device_id}/down` (see module docstring).
        `obj` is the down envelope's fields (no `n`, no `sig` -- those are
        this method's job), the same shape `wire.DownEnvelope` models.
        Returns `False` (never raises) on anything that stops the publish:
        an unregistered device, an `authMode: "hmac"` device with no
        `deviceSecrets` row, or `self.publish()` itself failing -- every one
        of those leaves the caller's delivery `queued` for a later retry."""
        device = devices_store.get_device(device_id)
        if device is None:
            logger.warning("publish_down: no such device %s", device_id)
            return False
        topic = f"pager/{device_id}/down"
        # DEVICE_PLAN.md §2.4: answer in the encoding of the device's last
        # accepted inbound envelope; `None` (no envelope accepted yet)
        # defaults to JSON, §2.4's own "logs and humans" default.
        wire_encoding = device.wire or "json"
        if device.authMode == "hmac":
            secret = device_secrets_store.get(device_id)
            if secret is None:
                # See module docstring: fails closed, same as ingest.py's
                # inbound verification when a secret is missing.
                logger.error(
                    "publish_down: device %s is authMode=hmac with no "
                    "deviceSecrets row -- dropping (delivery stays queued)",
                    device_id,
                )
                return False
            n = device_secrets_store.next_down_n(device_id)
            signed_obj = {**obj, "n": n}
            if wire_encoding == "cbor":
                payload = devauth.sign_cbor(secret.hmacKey, topic, signed_obj)
            else:
                payload = devauth.sign_json(secret.hmacKey, topic, signed_obj)
        elif wire_encoding == "cbor":
            payload = wirecbor.encode(obj)
        else:
            payload = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        return self.publish(topic, payload, qos=1, retain=False)

    def healthcheck(self) -> bool:
        """`GET {base}/status` --
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
        qos = data.get("qos", 0)
        if not isinstance(topic, str):
            return None
        payload_b64 = data.get("payload_b64")
        if payload_b64 is not None:
            # The authoritative field (module docstring): the only one that
            # survives a non-UTF-8 payload. Malformed base64 means the
            # broker sent something this relay cannot interpret -- drop it
            # rather than fall back to the lossy `payload`, which would
            # hand `ingest` bytes that are silently not what the device
            # sent (and would fail signature verification anyway).
            if not isinstance(payload_b64, str):
                return None
            try:
                payload_bytes = base64.b64decode(payload_b64, validate=True)
            except (binascii.Error, ValueError):
                return None
        else:
            payload = data.get("payload")
            if isinstance(payload, str):
                payload_bytes = payload.encode("utf-8")
            elif isinstance(payload, (bytes, bytearray)):
                payload_bytes = bytes(payload)
            else:
                return None
        if not isinstance(qos, int) or isinstance(qos, bool):
            return None
        return topic, payload_bytes, qos

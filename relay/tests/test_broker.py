"""`app/broker.py`'s two binary-safety contracts, which nothing else covered.

Both halves of the same bug (fixed together): the relay talks to the broker
over JSON in *both* directions, and an MQTT payload is not text. A CBOR
envelope (docs/PROTOCOL.md §3.1), any HMAC signature inside one (§14), and
S2b.1's AES-GCM bootstrap blob are all arbitrary bytes.

Before the fix, `publish()` did `payload.decode("utf-8")`, which raised
`UnicodeDecodeError` on every one of those -- 500ing `POST
/api/admin/devices` once `devsetup.issue()` was wired in for real -- and
`parse_webhook()` read the rule-engine event's `payload` field, which EMQX
has already mangled to U+FFFD by the time the relay sees it.
"""

from __future__ import annotations

import base64
import json
from typing import Any

import httpx
import pytest

from app.broker import BrokerClient
from app.config import Settings

# Deliberately not valid UTF-8 anywhere in it: 0xdd is the exact byte from
# the reported traceback, and 0x00/0xff/0xa3 are ordinary CBOR bytes.
BINARY_PAYLOAD = bytes([0xA3, 0xDD, 0x00, 0xFF, 0x7B, 0x22]) + "héllo".encode()
JSON_PAYLOAD = b'{"v":1,"id":"m_00000001"}'


def make_broker(**overrides: Any) -> BrokerClient:
    defaults: dict[str, Any] = {
        "broker_api_url": "http://broker.invalid/api/v5",
        "broker_api_key": "key",
        "broker_api_secret": "secret",
        "webhook_key": "test-webhook-key",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return BrokerClient(Settings(**defaults))


@pytest.fixture
def sent(monkeypatch: pytest.MonkeyPatch) -> list[dict[str, Any]]:
    """Captures the JSON body of every `httpx.post` the broker client makes."""
    calls: list[dict[str, Any]] = []

    def fake_post(url: str, **kwargs: Any) -> httpx.Response:
        calls.append({"url": url, **kwargs})
        return httpx.Response(200, request=httpx.Request("POST", url))

    monkeypatch.setattr(httpx, "post", fake_post)
    return calls


# ---- outbound: publish() ----


@pytest.mark.parametrize("payload", [BINARY_PAYLOAD, JSON_PAYLOAD, b""])
def test_publish_sends_base64_for_every_payload(sent: list[dict[str, Any]], payload: bytes):
    # One code path, not "base64 only when it isn't text": the binary case
    # must not be the rarely-exercised branch.
    assert make_broker().publish("pager/pgr-0001/down", payload, qos=1, retain=False) is True
    body = sent[0]["json"]
    assert body["payload_encoding"] == "base64"
    assert base64.b64decode(body["payload"], validate=True) == payload
    assert body["topic"] == "pager/pgr-0001/down"
    assert body["qos"] == 1 and body["retain"] is False


def test_publish_binary_does_not_raise_and_reports_success(sent: list[dict[str, Any]]):
    # The reported traceback: devsetup.issue()'s retained AES-GCM bootstrap
    # blob on /boot/{bid}/down. This used to raise UnicodeDecodeError out of
    # publish() and 500 the admin endpoint.
    assert make_broker().publish("boot/bid-1/down", BINARY_PAYLOAD, qos=1, retain=True) is True
    assert sent[0]["json"]["retain"] is True


def test_publish_returns_false_on_transport_error(monkeypatch: pytest.MonkeyPatch):
    def boom(url: str, **kwargs: Any) -> httpx.Response:
        raise httpx.ConnectError("nope", request=httpx.Request("POST", url))

    monkeypatch.setattr(httpx, "post", boom)
    assert make_broker().publish("pager/pgr-0001/down", BINARY_PAYLOAD, 1, False) is False


def test_publish_returns_false_on_rejection(monkeypatch: pytest.MonkeyPatch):
    def fake_post(url: str, **kwargs: Any) -> httpx.Response:
        return httpx.Response(400, text="bad", request=httpx.Request("POST", url))

    monkeypatch.setattr(httpx, "post", fake_post)
    assert make_broker().publish("pager/pgr-0001/down", JSON_PAYLOAD, 1, False) is False


# ---- inbound: parse_webhook() ----


def emqx_body(topic: str, payload: bytes, qos: int = 1) -> bytes:
    """What EMQX 5.8.0 actually POSTs for `"body": "${.}"` with the
    `base64_encode(payload) as payload_b64` rule SQL: the lossy `payload`
    *and* the exact `payload_b64`, side by side."""
    return json.dumps(
        {
            "topic": topic,
            "payload": payload.decode("utf-8", "replace"),
            "payload_b64": base64.b64encode(payload).decode("ascii"),
            "qos": qos,
            "clientid": "pgr-0001",
        }
    ).encode("utf-8")


@pytest.mark.parametrize("payload", [BINARY_PAYLOAD, JSON_PAYLOAD, b""])
def test_parse_webhook_recovers_exact_bytes(payload: bytes):
    parsed = BrokerClient.parse_webhook(emqx_body("pager/pgr-0001/up", payload))
    assert parsed == ("pager/pgr-0001/up", payload, 1)


def test_parse_webhook_ignores_the_lossy_payload_field():
    # Proves the fix is reading `payload_b64`, not `payload`: EMQX's own
    # rendering of these bytes is unrecoverable, so a parser that trusted it
    # would return something shorter/different here.
    body = BrokerClient.parse_webhook(emqx_body("pager/pgr-0001/up", BINARY_PAYLOAD))
    assert body is not None
    assert body[1] == BINARY_PAYLOAD
    assert body[1] != BINARY_PAYLOAD.decode("utf-8", "replace").encode("utf-8")


def test_parse_webhook_falls_back_to_plain_payload():
    # A broker still provisioned by an older tools/emqx_setup.py (no
    # payload_b64 in the rule SQL) must keep working for text envelopes
    # rather than having every webhook dropped.
    body = json.dumps({"topic": "pager/pgr-0001/up", "payload": '{"v":1}', "qos": 1}).encode()
    assert BrokerClient.parse_webhook(body) == ("pager/pgr-0001/up", b'{"v":1}', 1)


@pytest.mark.parametrize(
    "body",
    [
        b"not json at all",
        b"[1,2,3]",
        json.dumps({"payload_b64": "AAA="}).encode(),  # no topic
        json.dumps({"topic": "pager/pgr-0001/up"}).encode(),  # no payload of any kind
        json.dumps({"topic": "t", "payload_b64": "!!!not base64!!!"}).encode(),
        json.dumps({"topic": "t", "payload_b64": 17}).encode(),
        json.dumps({"topic": "t", "payload_b64": "AAA=", "qos": "one"}).encode(),
        json.dumps({"topic": "t", "payload": 17}).encode(),
    ],
)
def test_parse_webhook_rejects_malformed_bodies(body: bytes):
    # None means "2xx but dispatch nothing" (PROTOCOL.md §3.4) -- never an
    # exception, and never a silently-wrong payload.
    assert BrokerClient.parse_webhook(body) is None


def test_parse_webhook_does_not_fall_back_when_b64_is_broken():
    # If the authoritative field is unusable, the lossy one is not a
    # substitute: handing ingest bytes that are not what the device sent
    # would fail signature verification and count against sigFailures.
    body = json.dumps(
        {"topic": "pager/pgr-0001/up", "payload": '{"v":1}', "payload_b64": "%%%"}
    ).encode()
    assert BrokerClient.parse_webhook(body) is None

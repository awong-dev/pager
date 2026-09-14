"""HTTP API tests: bearer auth, POST validation/publish, GET thread/status."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from tests.fake_transport import FakeBrokerClient

TOKEN = "test-token-123"


@pytest.fixture
def client(tmp_path) -> Iterator[TestClient]:
    settings = Settings(
        relay_token=TOKEN,
        broker_api_url="http://unused.invalid/api/v5",
        broker_api_key=None,
        broker_api_secret=None,
        webhook_key="test-webhook-key",
        db_path=str(tmp_path / "relay.db"),
    )
    fake_broker = FakeBrokerClient()

    app = create_app(settings=settings, broker_client=fake_broker)
    with TestClient(app) as c:
        c.fake_broker = fake_broker  # type: ignore[attr-defined]
        yield c


def auth_headers(token: str = TOKEN) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


# ---- auth ----


def test_missing_token_is_401(client: TestClient):
    resp = client.get("/api/devices/pgr-0001/status")
    assert resp.status_code == 401


def test_wrong_token_is_401(client: TestClient):
    resp = client.get("/api/devices/pgr-0001/status", headers=auth_headers("wrong"))
    assert resp.status_code == 401


def test_correct_token_is_authorized(client: TestClient):
    resp = client.get("/api/devices/pgr-0001/status", headers=auth_headers())
    assert resp.status_code == 404  # authorized, just no status seen yet


# ---- POST /messages ----


def test_send_message_publishes_and_reaches_sent_state(client: TestClient):
    resp = client.post(
        "/api/devices/pgr-0001/messages",
        json={"body": "Pickup at 3:15 by the gym"},
        headers=auth_headers(),
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["id"].startswith("m_")
    assert len(data["id"]) == 10
    # BrokerClient.publish() is a synchronous REST call in this design (no
    # separate PUBACK step) -- a successful publish is 'sent' immediately.
    assert data["state"] == "sent"

    fake_broker: FakeBrokerClient = client.fake_broker  # type: ignore[attr-defined]
    assert len(fake_broker.published) == 1
    assert fake_broker.published[0].topic == "pager/pgr-0001/down"

    thread = client.get("/api/devices/pgr-0001/messages", headers=auth_headers()).json()
    assert len(thread) == 1
    assert thread[0]["state"] == "sent"


def test_send_message_stays_queued_when_broker_publish_fails(client: TestClient):
    fake_broker: FakeBrokerClient = client.fake_broker  # type: ignore[attr-defined]
    fake_broker.fail_publish = True

    resp = client.post(
        "/api/devices/pgr-0001/messages", json={"body": "hi"}, headers=auth_headers()
    )
    assert resp.status_code == 200
    assert resp.json()["state"] == "queued"


def test_send_message_requires_auth(client: TestClient):
    resp = client.post("/api/devices/pgr-0001/messages", json={"body": "hi"})
    assert resp.status_code == 401


def test_send_message_rejects_invalid_device_id(client: TestClient):
    resp = client.post(
        "/api/devices/BAD ID/messages", json={"body": "hi"}, headers=auth_headers()
    )
    assert resp.status_code == 400


def test_send_message_strips_control_chars(client: TestClient):
    resp = client.post(
        "/api/devices/pgr-0001/messages",
        json={"body": "hi\x01\x02 there\x7f"},
        headers=auth_headers(),
    )
    assert resp.status_code == 200
    fake_broker: FakeBrokerClient = client.fake_broker  # type: ignore[attr-defined]
    assert b"hi there" in fake_broker.published[-1].payload


def test_send_message_rejects_empty_after_stripping(client: TestClient):
    resp = client.post(
        "/api/devices/pgr-0001/messages",
        json={"body": "\x01\x02\x03"},
        headers=auth_headers(),
    )
    assert resp.status_code == 400


def test_send_message_rejects_over_160_codepoints(client: TestClient):
    resp = client.post(
        "/api/devices/pgr-0001/messages",
        json={"body": "a" * 161},
        headers=auth_headers(),
    )
    assert resp.status_code == 400


def test_send_message_rejects_over_320_utf8_bytes(client: TestClient):
    # Each of these is 1 codepoint but 3 UTF-8 bytes -> 120 chars = 360 bytes,
    # under the 160-codepoint cap but over the 320-byte cap.
    body = "☃" * 120
    resp = client.post(
        "/api/devices/pgr-0001/messages",
        json={"body": body},
        headers=auth_headers(),
    )
    assert resp.status_code == 400


def test_get_messages_opportunistically_retries_queued_message(client: TestClient):
    """A stand-in for docs/SERVER_PLAN.md §5.8's `/internal/tick`: a message
    that stayed 'queued' because the broker publish failed gets retried the
    next time anyone reads the thread (see Ingest.retry_queued)."""
    fake_broker: FakeBrokerClient = client.fake_broker  # type: ignore[attr-defined]
    fake_broker.fail_publish = True

    sent = client.post(
        "/api/devices/pgr-0001/messages", json={"body": "outage"}, headers=auth_headers()
    ).json()
    assert sent["state"] == "queued"

    fake_broker.fail_publish = False
    thread = client.get("/api/devices/pgr-0001/messages", headers=auth_headers()).json()
    assert len(thread) == 1
    assert thread[0]["state"] == "sent"


# ---- GET /messages ----


def test_get_messages_empty_thread(client: TestClient):
    resp = client.get("/api/devices/pgr-0001/messages", headers=auth_headers())
    assert resp.status_code == 200
    assert resp.json() == []


def test_get_messages_since_filters(client: TestClient):
    import time

    client.post("/api/devices/pgr-0001/messages", json={"body": "first"}, headers=auth_headers())
    cutoff = int(time.time())
    time.sleep(1.1)  # guarantee the second message's created_at > cutoff
    client.post("/api/devices/pgr-0001/messages", json={"body": "second"}, headers=auth_headers())

    resp = client.get(f"/api/devices/pgr-0001/messages?since={cutoff}", headers=auth_headers())
    bodies = [m["body"] for m in resp.json()]
    assert bodies == ["second"]


# ---- GET /status ----


def test_get_status_404_when_never_seen(client: TestClient):
    resp = client.get("/api/devices/pgr-9999/status", headers=auth_headers())
    assert resp.status_code == 404


# ---- static parent page ----


def test_root_serves_parent_page(client: TestClient):
    resp = client.get("/")
    assert resp.status_code == 200
    assert "text/html" in resp.headers["content-type"]
    assert "Pager Parent Console" in resp.text
    # No auth required to load the shell -- it can't do anything without a
    # token/device id entered client-side, and the API routes underneath it
    # are still bearer-gated.
    assert resp.status_code != 401


def test_root_page_does_not_shadow_api_routes(client: TestClient):
    resp = client.get("/api/devices/pgr-0001/status")
    assert resp.status_code == 401  # auth still enforced, not swallowed by the static mount

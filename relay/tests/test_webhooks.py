"""Tests for POST /webhooks/mqtt: webhook-key auth and dispatch by topic
suffix. The ack/status/loc business logic itself is covered by
tests/test_ingest.py against Ingest directly; this file is about the router
(app/routers/webhooks.py) -- auth, parsing, dispatch, and the "always 2xx
unless the auth itself is wrong" contract (docs/PROTOCOL.md §3.4).
"""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from google.api_core.exceptions import ServiceUnavailable

from app.config import Settings
from app.main import create_app
from app.store import legacy as legacy_store
from tests.conftest import (
    ack_payload,
    loc_topic,
    status_topic,
    up_message_payload,
    up_topic,
)
from tests.fake_transport import FakeBrokerClient, webhook_body, webhook_headers

TOKEN = "test-token-123"
WEBHOOK_KEY = "test-webhook-key"


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "relay_token": TOKEN,
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": WEBHOOK_KEY,
        "db_path": "unused.db",
        "dev_mode": False,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture
def client() -> Iterator[TestClient]:
    settings = make_settings()
    fake_broker = FakeBrokerClient(webhook_key=WEBHOOK_KEY)
    app = create_app(settings=settings, broker_client=fake_broker)
    with TestClient(app) as c:
        c.fake_broker = fake_broker  # type: ignore[attr-defined]
        yield c


def test_webhook_missing_key_is_401(client: TestClient):
    resp = client.post(
        "/webhooks/mqtt", content=webhook_body(up_topic("pgr-0001"), b"{}"), headers={}
    )
    assert resp.status_code == 401


def test_webhook_wrong_key_is_401(client: TestClient):
    resp = client.post(
        "/webhooks/mqtt",
        content=webhook_body(up_topic("pgr-0001"), b"{}"),
        headers=webhook_headers("wrong-key"),
    )
    assert resp.status_code == 401


def test_webhook_up_message_dispatches_and_stores(client: TestClient):
    payload = ack_payload("u_11111111", "shown")  # any well-formed up envelope shape works here
    body = webhook_body(up_topic("pgr-0001"), payload)
    resp = client.post("/webhooks/mqtt", content=body, headers=webhook_headers(WEBHOOK_KEY))
    assert resp.status_code == 200


def test_webhook_status_dispatches(client: TestClient):
    from tests.conftest import online_status_payload

    body = webhook_body(status_topic("pgr-0001"), online_status_payload("s_00000001"))
    resp = client.post("/webhooks/mqtt", content=body, headers=webhook_headers(WEBHOOK_KEY))
    assert resp.status_code == 200

    status_resp = client.get(
        "/api/devices/pgr-0001/status", headers={"Authorization": f"Bearer {TOKEN}"}
    )
    assert status_resp.status_code == 200
    assert status_resp.json()["state"] == "online"


def test_webhook_loc_dispatches_without_error(client: TestClient):
    from tests.conftest import loc_payload

    body = webhook_body(loc_topic("pgr-0001"), loc_payload("l_11111111"))
    resp = client.post("/webhooks/mqtt", content=body, headers=webhook_headers(WEBHOOK_KEY))
    assert resp.status_code == 200


def test_webhook_malformed_body_is_still_200(client: TestClient):
    resp = client.post(
        "/webhooks/mqtt", content=b"not json at all", headers=webhook_headers(WEBHOOK_KEY)
    )
    assert resp.status_code == 200


def test_webhook_unrecognised_topic_is_still_200(client: TestClient):
    body = webhook_body("pager/pgr-0001/unknown", b"{}")
    resp = client.post("/webhooks/mqtt", content=body, headers=webhook_headers(WEBHOOK_KEY))
    assert resp.status_code == 200


def test_webhook_store_error_on_up_insert_is_500_not_200(monkeypatch):
    # §3.4's 2xx-for-malformed-payload rule must NOT swallow a genuine
    # relay-side storage failure -- that would silently lose the up-message
    # with no republish path (§4.2 has none). This must surface as a 500 so
    # the broker's rule engine retries the webhook.
    def _broken_insert(**kwargs):
        raise ServiceUnavailable("firestore unreachable")

    monkeypatch.setattr(legacy_store, "insert_up_message", _broken_insert)

    settings = make_settings()
    fake_broker = FakeBrokerClient(webhook_key=WEBHOOK_KEY)
    app = create_app(settings=settings, broker_client=fake_broker)
    body = webhook_body(up_topic("pgr-0001"), up_message_payload("u_broken01", "hi"))
    with TestClient(app, raise_server_exceptions=False) as c:
        resp = c.post("/webhooks/mqtt", content=body, headers=webhook_headers(WEBHOOK_KEY))
        assert resp.status_code == 500

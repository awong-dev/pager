"""`GET /healthz` -- docs/SERVER_PLAN.md §5.1: "200 + firestore reachable +
broker API reachable".
check Firestore only; see `app/main.py`'s module-level comment on `healthz`
for why broker reachability is now also checked, per the doc."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from tests.fake_transport import FakeBrokerClient


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "test-webhook-key",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def client(broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        yield c


def test_healthz_ok_when_firestore_and_broker_both_reachable(client: TestClient):
    resp = client.get("/healthz")
    assert resp.status_code == 200
    body = resp.json()
    assert body == {"ok": True, "firestore": True, "broker": True}


def test_healthz_503_when_broker_unreachable(client: TestClient, broker: FakeBrokerClient):
    broker.healthy = False
    resp = client.get("/healthz")
    assert resp.status_code == 503
    body = resp.json()
    assert body["ok"] is False
    assert body["broker"] is False
    assert body["firestore"] is True


def test_healthz_503_when_firestore_unreachable(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    import app.main as main_module

    def _raise_firestore_error():
        raise RuntimeError("firestore unreachable (simulated)")

    monkeypatch.setattr(main_module, "get_db", _raise_firestore_error)
    resp = client.get("/healthz")
    assert resp.status_code == 503
    body = resp.json()
    assert body["ok"] is False
    assert body["firestore"] is False
    assert body["broker"] is True


def test_healthz_response_carries_a_request_id_header(client: TestClient):
    resp = client.get("/healthz")
    assert resp.status_code == 200
    assert resp.headers.get("X-Request-Id")


def test_healthz_echoes_a_caller_supplied_request_id(client: TestClient):
    resp = client.get("/healthz", headers={"X-Request-Id": "caller-supplied-id-123"})
    assert resp.headers.get("X-Request-Id") == "caller-supplied-id-123"

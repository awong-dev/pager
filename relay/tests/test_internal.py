"""`/internal/tick`, `/internal/sweep` -- docs/SERVER_PLAN.md §5.1, §5.8.
No OIDC yet (Phase 8); gated on `DEV_MODE` (404 when off, matching
`app/routers/dev.py`'s pattern)."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "relay_token": "unused",
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "test-webhook-key",
        "db_path": "unused.db",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture
def client() -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        yield c


def test_tick_not_found_when_dev_mode_off():
    app = create_app(settings=make_settings(dev_mode=False), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        resp = c.post("/internal/tick")
        assert resp.status_code == 404


def test_sweep_not_found_when_dev_mode_off():
    app = create_app(settings=make_settings(dev_mode=False), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        resp = c.post("/internal/sweep")
        assert resp.status_code == 404


def test_sweep_runs_and_reports_zero_deletions_with_nothing_stale(client: TestClient):
    """Real retention logic lives in `app/jobs.py` (`tests/test_jobs.py`
    covers its batching/idempotency/unit-conversion in depth) -- this is
    just the route wiring: `POST /internal/sweep` calls it and returns its
    `SweepResult`, which is all zeros when nothing in a fresh emulator is
    old enough to sweep."""
    resp = client.post("/internal/sweep")
    assert resp.status_code == 200
    assert resp.json() == {
        "messagesDeleted": 0,
        "wireIdsDeleted": 0,
        "locationsDeleted": 0,
        "locWireIdsDeleted": 0,
        "locReqsDeleted": 0,
        "orphanedWireIdsDeleted": 0,
        "conversationsDeleted": 0,
    }


def test_tick_retries_queued_pager_deliveries_end_to_end():
    from tests.firebase_test_utils import auth_header

    fb_auth.create_user(uid="mom", email="mom-tick@example.com")
    users_store.create_user(uid="mom", alias="mom", display_name="mom")
    users_store.create_user(uid="kid", alias="kid", display_name="kid")
    allow_store.set_edge("mom", "kid", message=True, locate=True)
    devices_store.create_device(
        device_id="pgr-tick-int",
        owner_uid="kid",
        label="d",
        mqtt_username="pgr-tick-int",
        mqtt_password_hash="x",
    )
    backends_store.create_backend(
        "kid", kind="pager", config={"deviceId": "pgr-tick-int"}, enabled=True
    )

    # Constructs its own app (rather than using the `client` fixture) so the
    # test keeps a direct reference to the `FakeBrokerClient` it passed in --
    # `TestClient` does not reliably expose `app.state` back out.
    broker = FakeBrokerClient()
    broker.fail_publish = True
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        send_resp = c.post(
            "/api/conversations/kid/messages",
            json={"body": "hi"},
            headers=auth_header("mom"),
        )
        assert send_resp.status_code == 201

        broker.fail_publish = False
        tick_resp = c.post("/internal/tick")
        assert tick_resp.status_code == 200
        data = tick_resp.json()
        assert data["retriesAttempted"] == 1
        assert len(broker.published) == 1

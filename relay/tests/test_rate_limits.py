"""`app/store/rate_limits.py` and its call sites -- docs/SERVER_PLAN.md Phase
rate limits: `POST
/api/me/backends` (highest priority -- can trigger a real SMS send),
`/api/admin/*` writes (coarser, defense-in-depth), and a per-IP cap on
`/webhooks/twilio/sms`/`/webhooks/gchat` (defense-in-depth against a flood
of forged-but-cheap-to-generate requests).
"""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import rate_limits as rate_limits_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

# ---------------------------------------------------------------------------
# store-level: app.store.rate_limits.check_and_increment
# ---------------------------------------------------------------------------


def test_check_and_increment_allows_up_to_the_limit_then_trips():
    key = "unit-test-limit"
    now = 1_000_000.0
    for _ in range(3):
        assert rate_limits_store.check_and_increment(key, limit=3, window_s=60, now=now) is True
    # The 4th call within the same window is rejected.
    assert rate_limits_store.check_and_increment(key, limit=3, window_s=60, now=now) is False


def test_check_and_increment_does_not_count_a_rejected_call():
    key = "unit-test-rejected-not-counted"
    now = 1_000_000.0
    rate_limits_store.check_and_increment(key, limit=1, window_s=60, now=now)
    # Trips.
    assert rate_limits_store.check_and_increment(key, limit=1, window_s=60, now=now) is False
    # Still trips -- the rejected call above must not itself have incremented
    # the counter (or repeated 429s would eventually "use up" a slot that
    # was never actually granted).
    assert rate_limits_store.check_and_increment(key, limit=1, window_s=60, now=now) is False


def test_check_and_increment_resets_after_the_window_elapses():
    key = "unit-test-window-reset"
    now = 1_000_000.0
    assert rate_limits_store.check_and_increment(key, limit=1, window_s=60, now=now) is True
    assert rate_limits_store.check_and_increment(key, limit=1, window_s=60, now=now) is False
    # A fresh window (61s later) allows again.
    assert (
        rate_limits_store.check_and_increment(key, limit=1, window_s=60, now=now + 61) is True
    )


# ---------------------------------------------------------------------------
# POST /api/me/backends
# ---------------------------------------------------------------------------


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
def client() -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        yield c


@pytest.fixture(autouse=True)
def _tight_backend_create_limit(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("RATE_LIMIT_BACKEND_CREATE_LIMIT", "2")
    monkeypatch.setenv("RATE_LIMIT_BACKEND_CREATE_WINDOW_S", "3600")


def _make_user(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias)
    return auth_header(uid)


def test_backend_create_trips_429_after_the_configured_limit(client: TestClient):
    headers = _make_user("rl1", "rl1")
    for i in range(2):
        resp = client.post(
            "/api/me/backends",
            json={"kind": "sms", "config": {"phone": f"+1555000000{i}"}, "enabled": False},
            headers=headers,
        )
        assert resp.status_code == 200, resp.text
    resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15550000099"}, "enabled": False},
        headers=headers,
    )
    assert resp.status_code == 429


def test_backend_create_rate_limit_is_per_user(client: TestClient):
    headers1 = _make_user("rl2", "rl2")
    headers2 = _make_user("rl3", "rl3")
    for i in range(2):
        resp = client.post(
            "/api/me/backends",
            json={"kind": "sms", "config": {"phone": f"+1555000010{i}"}, "enabled": False},
            headers=headers1,
        )
        assert resp.status_code == 200
    # A different user's own quota is untouched by the first user's calls.
    resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15550000199"}, "enabled": False},
        headers=headers2,
    )
    assert resp.status_code == 200


def test_backend_create_allows_again_after_the_window_resets(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    headers = _make_user("rl4", "rl4")
    monkeypatch.setenv("RATE_LIMIT_BACKEND_CREATE_WINDOW_S", "0")
    for i in range(2):
        resp = client.post(
            "/api/me/backends",
            json={"kind": "sms", "config": {"phone": f"+1555000020{i}"}, "enabled": False},
            headers=headers,
        )
        assert resp.status_code == 200
    # window_s=0 -- every call is effectively its own fresh window, so a
    # 3rd call still succeeds rather than tripping.
    resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15550000299"}, "enabled": False},
        headers=headers,
    )
    assert resp.status_code == 200


# ---------------------------------------------------------------------------
# /api/admin/* writes
# ---------------------------------------------------------------------------


@pytest.fixture(autouse=True)
def _tight_admin_write_limit(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("RATE_LIMIT_ADMIN_WRITE_LIMIT", "2")
    monkeypatch.setenv("RATE_LIMIT_ADMIN_WRITE_WINDOW_S", "3600")


@pytest.fixture
def admin_headers() -> dict[str, str]:
    auth_user = fb_auth.create_user(email="rl-admin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="rladmin", display_name="RL Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    return auth_header(auth_user.uid)


def test_admin_write_trips_429_after_the_configured_limit(
    client: TestClient, admin_headers: dict[str, str]
):
    current = client.get("/api/admin/settings", headers=admin_headers).json()
    for _ in range(2):
        resp = client.put("/api/admin/settings", json=current, headers=admin_headers)
        assert resp.status_code == 200, resp.text
    resp = client.put("/api/admin/settings", json=current, headers=admin_headers)
    assert resp.status_code == 429


def test_admin_reads_are_not_rate_limited(client: TestClient, admin_headers: dict[str, str]):
    """Only writes carry `require_admin_write_rate_limit` -- `GET` routes
    stay uncapped by this control (they're the cheapest, least dangerous
    thing this router does)."""
    for _ in range(5):
        resp = client.get("/api/admin/settings", headers=admin_headers)
        assert resp.status_code == 200


def test_admin_write_limit_shared_across_routes_for_the_same_admin(
    client: TestClient, admin_headers: dict[str, str]
):
    """The limit is keyed on the admin's uid, not per-route -- one write to
    `/api/admin/settings` and one to `/api/admin/allowlist` together exhaust
    the same 2-request budget."""
    current = client.get("/api/admin/settings", headers=admin_headers).json()
    resp1 = client.put("/api/admin/settings", json=current, headers=admin_headers)
    assert resp1.status_code == 200
    resp2 = client.put("/api/admin/allowlist", json={"entries": []}, headers=admin_headers)
    assert resp2.status_code == 200
    resp3 = client.put("/api/admin/settings", json=current, headers=admin_headers)
    assert resp3.status_code == 429


# ---------------------------------------------------------------------------
# Per-IP cap on /webhooks/twilio/sms and /webhooks/gchat
# ---------------------------------------------------------------------------


@pytest.fixture(autouse=True)
def _tight_webhook_ip_limit(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("RATE_LIMIT_WEBHOOK_IP_LIMIT", "2")
    monkeypatch.setenv("RATE_LIMIT_WEBHOOK_IP_WINDOW_S", "3600")


def make_webhook_settings(**overrides: object) -> Settings:
    defaults = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "test-webhook-key",
        "dev_mode": False,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture
def webhook_client() -> Iterator[TestClient]:
    app = create_app(settings=make_webhook_settings(), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        yield c


def test_twilio_webhook_ip_cap_trips_429_before_signature_check(webhook_client: TestClient):
    # No TWILIO_AUTH_TOKEN configured -- every one of these would 401 on the
    # signature check if it ever got that far. The point of this test is
    # that the 3rd request from the same IP is rejected by the rate limiter
    # (429), not the signature check (401), because the IP check runs first.
    params = {"To": "+15005550006", "From": "+15551234567", "Body": "hi"}
    for _ in range(2):
        resp = webhook_client.post("/webhooks/twilio/sms", data=params)
        assert resp.status_code == 401
    resp = webhook_client.post("/webhooks/twilio/sms", data=params)
    assert resp.status_code == 429


def test_gchat_webhook_ip_cap_trips_429_before_jwt_check(webhook_client: TestClient):
    for _ in range(2):
        resp = webhook_client.post("/webhooks/gchat", json={"type": "MESSAGE"})
        assert resp.status_code == 401
    resp = webhook_client.post("/webhooks/gchat", json={"type": "MESSAGE"})
    assert resp.status_code == 429


def test_webhook_ip_cap_is_independent_per_endpoint_bucket(webhook_client: TestClient):
    """`/webhooks/twilio/sms` and `/webhooks/gchat` are rate limited
    independently (`webhook_ip:sms:...` vs `webhook_ip:gchat:...`) -- even
    though `TestClient` always presents the same source IP, exhausting one
    endpoint's budget must not affect the other's."""
    params = {"To": "+15005550006", "From": "+15551234567", "Body": "hi"}
    for _ in range(2):
        resp = webhook_client.post("/webhooks/twilio/sms", data=params)
        assert resp.status_code == 401
    resp = webhook_client.post("/webhooks/twilio/sms", data=params)
    assert resp.status_code == 429

    # gchat's own budget is untouched.
    resp = webhook_client.post("/webhooks/gchat", json={"type": "MESSAGE"})
    assert resp.status_code == 401


def test_webhook_ip_cap_keys_on_x_forwarded_for_not_the_proxy_peer(webhook_client: TestClient):
    """Every real request reaches these routes
    through Firebase Hosting -> Cloud Run, so `request.client.host` is one
    shared Google front-end address; keying on it would let one flooder 429
    all legitimate Twilio/Chat traffic. `_client_ip` keys on
    `X-Forwarded-For`'s originating entry instead, so exhausting one source
    IP's budget leaves another source IP's untouched."""
    params = {"To": "+15005550006", "From": "+15551234567", "Body": "hi"}
    for _ in range(2):
        resp = webhook_client.post(
            "/webhooks/twilio/sms", data=params, headers={"X-Forwarded-For": "203.0.113.9, 10.0.0.1"}
        )
        assert resp.status_code == 401
    resp = webhook_client.post(
        "/webhooks/twilio/sms", data=params, headers={"X-Forwarded-For": "203.0.113.9, 10.0.0.1"}
    )
    assert resp.status_code == 429
    # A different originating IP behind the same proxy still has its budget.
    resp = webhook_client.post(
        "/webhooks/twilio/sms", data=params, headers={"X-Forwarded-For": "198.51.100.4, 10.0.0.1"}
    )
    assert resp.status_code == 401

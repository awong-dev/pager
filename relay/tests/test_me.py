"""`/api/me/*` -- docs/SERVER_PLAN.md §5.1."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header


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


def _make_user(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias)
    return auth_header(uid)


def test_get_me_returns_user_and_role(client: TestClient):
    headers = _make_user("me1", "me1")
    resp = client.get("/api/me", headers=headers)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["user"]["alias"] == "me1"
    assert data["role"] == "member"


def test_get_me_requires_auth(client: TestClient):
    resp = client.get("/api/me")
    assert resp.status_code == 401


def test_list_backends_includes_implicit_webapp_backend(client: TestClient):
    headers = _make_user("me2", "me2")
    resp = client.get("/api/me/backends", headers=headers)
    assert resp.status_code == 200
    kinds = {b["kind"] for b in resp.json()}
    assert kinds == {"webapp"}


def test_create_backend_rejects_pager_kind(client: TestClient):
    headers = _make_user("me3", "me3")
    resp = client.post(
        "/api/me/backends", json={"kind": "pager", "config": {}}, headers=headers
    )
    assert resp.status_code == 400


def test_create_update_delete_sms_backend(client: TestClient):
    headers = _make_user("me4", "me4")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15551234567"}, "enabled": False},
        headers=headers,
    )
    assert create_resp.status_code == 200, create_resp.text
    bid = create_resp.json()["id"]

    patch_resp = client.patch(
        f"/api/me/backends/{bid}", json={"enabled": True}, headers=headers
    )
    assert patch_resp.status_code == 200
    assert patch_resp.json()["enabled"] is True

    delete_resp = client.delete(f"/api/me/backends/{bid}", headers=headers)
    assert delete_resp.status_code == 200

    patch_after_delete = client.patch(
        f"/api/me/backends/{bid}", json={"enabled": True}, headers=headers
    )
    assert patch_after_delete.status_code == 404


def test_push_token_add_and_remove(client: TestClient):
    headers = _make_user("me5", "me5")
    add_resp = client.post(
        "/api/me/push-tokens", json={"token": "tok_abc123"}, headers=headers
    )
    assert add_resp.status_code == 200

    from app.store import push_tokens as push_tokens_store

    assert push_tokens_store.list_tokens("me5") == ["tok_abc123"]

    del_resp = client.delete("/api/me/push-tokens/tok_abc123", headers=headers)
    assert del_resp.status_code == 200
    assert push_tokens_store.list_tokens("me5") == []

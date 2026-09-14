"""`/api/admin/*` -- docs/SERVER_PLAN.md §5.1, §5.5: user/device CRUD, the
allow-list's replace-all semantics (including the `locatableBy` rewrite on
every affected device), and settings."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import devices as devices_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header



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


@pytest.fixture
def admin_headers() -> dict[str, str]:
    auth_user = fb_auth.create_user(email="root-admin@example.com")
    users_store.create_user(uid=auth_user.uid, alias="rootadmin", display_name="Root Admin", role="admin")
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    return auth_header(auth_user.uid)


# ---- users ----


def test_create_user_creates_auth_and_firestore_and_alias(
    client: TestClient, admin_headers: dict[str, str]
):
    resp = client.post(
        "/api/admin/users",
        json={"alias": "mom", "displayName": "Mom", "email": "mom@example.com"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["alias"] == "mom"
    assert data["role"] == "member"

    # Firebase Auth account really exists.
    auth_user = fb_auth.get_user_by_email("mom@example.com")
    assert auth_user.uid == data["uid"]
    # And the alias resolves.
    assert users_store.get_uid_for_alias("mom") == data["uid"]


def test_create_user_requires_email_or_phone(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post(
        "/api/admin/users",
        json={"alias": "nobody", "displayName": "Nobody"},
        headers=admin_headers,
    )
    assert resp.status_code == 400


def test_create_user_duplicate_alias_rolls_back_auth_user(
    client: TestClient, admin_headers: dict[str, str]
):
    client.post(
        "/api/admin/users",
        json={"alias": "dup", "displayName": "First", "email": "first@example.com"},
        headers=admin_headers,
    )
    resp = client.post(
        "/api/admin/users",
        json={"alias": "dup", "displayName": "Second", "email": "second@example.com"},
        headers=admin_headers,
    )
    assert resp.status_code == 400
    # The rolled-back Auth account must not linger.
    with pytest.raises(fb_auth.UserNotFoundError):
        fb_auth.get_user_by_email("second@example.com")


def test_patch_user_updates_fields_and_admin_claim(
    client: TestClient, admin_headers: dict[str, str]
):
    created = client.post(
        "/api/admin/users",
        json={"alias": "kid", "displayName": "Kid", "email": "kid@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.patch(
        f"/api/admin/users/{created['uid']}",
        json={"displayName": "Kiddo", "role": "admin"},
        headers=admin_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["displayName"] == "Kiddo"
    assert data["role"] == "admin"

    refreshed = fb_auth.get_user(created["uid"])
    assert refreshed.custom_claims and refreshed.custom_claims.get("admin") is True


def test_patch_user_404_for_unknown_uid(client: TestClient, admin_headers: dict[str, str]):
    resp = client.patch(
        "/api/admin/users/does-not-exist", json={"displayName": "x"}, headers=admin_headers
    )
    assert resp.status_code == 404


def test_delete_user_removes_auth_and_firestore(client: TestClient, admin_headers: dict[str, str]):
    created = client.post(
        "/api/admin/users",
        json={"alias": "gone", "displayName": "Gone", "email": "gone@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.delete(f"/api/admin/users/{created['uid']}", headers=admin_headers)
    assert resp.status_code == 200
    assert users_store.get_user(created["uid"]) is None
    with pytest.raises(fb_auth.UserNotFoundError):
        fb_auth.get_user(created["uid"])


# ---- devices ----


def test_create_device_returns_password_once_and_stores_only_a_hash(
    client: TestClient, admin_headers: dict[str, str]
):
    owner = client.post(
        "/api/admin/users",
        json={"alias": "owner1", "displayName": "Owner", "email": "owner1@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-1001", "ownerAlias": "owner1", "label": "test pager"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["mqttUsername"] == "pgr-1001"
    password = data["mqttPassword"]
    assert password

    device = devices_store.get_device("pgr-1001")
    assert device is not None
    assert device.ownerUid == owner["uid"]
    assert device.mqttPasswordHash != password  # never stored in the clear
    import hashlib

    assert device.mqttPasswordHash == hashlib.sha256(password.encode("utf-8")).hexdigest()


def test_create_device_picks_up_locatable_by_from_a_pre_existing_allow_edge(
    client: TestClient, admin_headers: dict[str, str]
):
    """An allow edge with `locate=True` set
    *before* the device it names as `toAlias` exists must not be lost --
    `devices_store.create_device` always starts a fresh device at
    `locatableBy: []`, and `set_edge`/`replace_all` only ever recompute
    `locatableBy` on devices that already exist at the moment an edge
    changes, so without `POST /api/admin/devices` recomputing it once more
    right after creation, this device would stay `locatableBy: []` forever
    (see `app.store.allow.recompute_locatable_by_for_owner`'s docstring)."""
    for alias in ("mom3", "kid3"):
        client.post(
            "/api/admin/users",
            json={"alias": alias, "displayName": alias, "email": f"{alias}@example.com"},
            headers=admin_headers,
        )
    mom_uid = users_store.get_uid_for_alias("mom3")

    # Allow edge set up *before* the device exists.
    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "mom3", "toAlias": "kid3", "message": True, "locate": True},
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    resp2 = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-5005", "ownerAlias": "kid3", "label": "kid device"},
        headers=admin_headers,
    )
    assert resp2.status_code == 200, resp2.text

    device = devices_store.get_device("pgr-5005")
    assert device.locatableBy == [mom_uid]


def test_create_device_unknown_owner_alias_is_400(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-2002", "ownerAlias": "ghost", "label": "x"},
        headers=admin_headers,
    )
    assert resp.status_code == 400


def test_delete_device(client: TestClient, admin_headers: dict[str, str]):
    client.post(
        "/api/admin/users",
        json={"alias": "owner2", "displayName": "Owner2", "email": "owner2@example.com"},
        headers=admin_headers,
    )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-3003", "ownerAlias": "owner2", "label": "x"},
        headers=admin_headers,
    )
    resp = client.delete("/api/admin/devices/pgr-3003", headers=admin_headers)
    assert resp.status_code == 200
    assert devices_store.get_device("pgr-3003") is None


# ---- allowlist replace-all + locatableBy rewrite ----


def test_allowlist_replace_all_rewrites_locatable_by(
    client: TestClient, admin_headers: dict[str, str]
):
    for alias in ("mom2", "dad2", "kid2"):
        client.post(
            "/api/admin/users",
            json={"alias": alias, "displayName": alias, "email": f"{alias}@example.com"},
            headers=admin_headers,
        )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-4004", "ownerAlias": "kid2", "label": "kid device"},
        headers=admin_headers,
    )

    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "mom2", "toAlias": "kid2", "message": True, "locate": True},
                {"fromAlias": "dad2", "toAlias": "kid2", "message": True, "locate": False},
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    entries = resp.json()
    assert len(entries) == 2

    mom_uid = users_store.get_uid_for_alias("mom2")
    dad_uid = users_store.get_uid_for_alias("dad2")

    device = devices_store.get_device("pgr-4004")
    # Only mom2 has locate=True -> only she appears in locatableBy.
    assert device.locatableBy == [mom_uid]

    # Now replace-all again, dropping mom2's edge and adding dad2's locate.
    resp2 = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "dad2", "toAlias": "kid2", "message": True, "locate": True},
            ]
        },
        headers=admin_headers,
    )
    assert resp2.status_code == 200
    device2 = devices_store.get_device("pgr-4004")
    assert device2.locatableBy == [dad_uid]


def test_allowlist_unknown_alias_is_400(client: TestClient, admin_headers: dict[str, str]):
    resp = client.put(
        "/api/admin/allowlist",
        json={"entries": [{"fromAlias": "ghost1", "toAlias": "ghost2"}]},
        headers=admin_headers,
    )
    assert resp.status_code == 400


# ---- settings ----


def test_put_and_get_settings(client: TestClient, admin_headers: dict[str, str]):
    resp = client.put(
        "/api/admin/settings",
        json={"messages": {"n": 2, "unit": "weeks"}, "locations": {"n": 3, "unit": "days"}},
        headers=admin_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["messages"] == {"n": 2, "unit": "weeks"}
    assert data["locations"] == {"n": 3, "unit": "days"}

    resp2 = client.get("/api/admin/settings", headers=admin_headers)
    assert resp2.json() == data


# ---- non-admin rejected ----


def test_non_admin_cannot_reach_admin_routes(client: TestClient):
    auth_user = fb_auth.create_user(email="plain@example.com")
    users_store.create_user(uid=auth_user.uid, alias="plain1", display_name="Plain")
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403

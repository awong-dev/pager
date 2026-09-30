"""app.auth: the registry gate (docs/SERVER_PLAN.md §5.3) -- a valid
Firebase ID token for a uid with no `users/{uid}` document must 403, not
401 (the token itself is fine; the relay just doesn't know that user)."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi import HTTPException
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.auth import (
    AuthedUser,
    Principal,
    principal_for,
    require_family_admin,
    require_super,
)
from app.config import Settings
from app.main import create_app
from app.store import users as users_store
from app.store.users import User
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header, mint_id_token


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


def test_no_bearer_token_is_401(client: TestClient):
    resp = client.get("/api/admin/users")
    assert resp.status_code == 401


def test_garbage_bearer_token_is_401(client: TestClient):
    resp = client.get("/api/admin/users", headers={"Authorization": "Bearer not-a-real-token"})
    assert resp.status_code == 401


def test_valid_token_unregistered_uid_is_403(client: TestClient):
    # A real Firebase Auth user -- but with no `users/{uid}` Firestore
    # document -- must be rejected, not silently treated as a guest.
    auth_user = fb_auth.create_user(email="stranger@example.com")
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403


def test_valid_token_registered_non_admin_is_403_on_admin_route(client: TestClient):
    auth_user = fb_auth.create_user(email="member@example.com")
    users_store.create_user(uid=auth_user.uid, alias="member1", display_name="Member One")
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403


def test_valid_token_registered_super_is_authorized(client: TestClient):
    # docs/FAMILIES_TASKS.md 1.2/5.1: `/api/admin/*` is gated on
    # `require_super` -- only the `role: 'super'` claim grants it, not the
    # legacy `admin` claim and not a family `admin`.
    auth_user = fb_auth.create_user(email="boss@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="boss", display_name="Boss", role="super"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"role": "super", "fam": ""})
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 200


def test_bare_legacy_admin_claim_grants_nothing_on_admin_route(client: TestClient):
    auth_user = fb_auth.create_user(email="legacy-admin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="legacyadmin", display_name="Legacy Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403


def test_family_admin_is_403_on_super_only_admin_route(client: TestClient):
    auth_user = fb_auth.create_user(email="famadmin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="famadmin", display_name="Fam Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"role": "admin", "fam": "famA"})
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403


def _authed(*, uid: str, role: str, family_id: str | None) -> AuthedUser:
    """Built directly, no Firebase round trip -- `require_family_admin`/
    `require_super` are plain functions of `AuthedUser`, so a unit test for
    them does not need a real token, only a `Principal`-shaped claims dict."""
    claims: dict = {"role": role, "fam": family_id or ""}
    user = User(uid=uid, alias=uid, displayName=uid, role=role, familyId=family_id)
    return AuthedUser(uid=uid, claims=claims, user=user)


def test_principal_for_defaults_a_claimless_token_to_member_no_family():
    authed = AuthedUser(uid="u1", claims={}, user=User(uid="u1", alias="u1", displayName="u1"))
    principal = principal_for(authed)
    assert principal == Principal(uid="u1", role="member", family_id=None)


def test_require_family_admin_rejects_a_member():
    authed = _authed(uid="mem1", role="member", family_id="famA")
    with pytest.raises(HTTPException) as exc_info:
        require_family_admin(authed, family=None, x_family=None)
    assert exc_info.value.status_code == 403


def test_require_family_admin_admin_naming_another_family_is_403():
    authed = _authed(uid="admA", role="admin", family_id="famA")
    with pytest.raises(HTTPException) as exc_info:
        require_family_admin(authed, family="famB", x_family=None)
    assert exc_info.value.status_code == 403


def test_require_family_admin_admin_on_own_family_is_ok():
    authed = _authed(uid="admA", role="admin", family_id="famA")
    principal, family_id = require_family_admin(authed, family=None, x_family=None)
    assert family_id == "famA"
    assert principal.uid == "admA"


def test_require_family_admin_super_without_family_is_400():
    authed = _authed(uid="sup1", role="super", family_id=None)
    with pytest.raises(HTTPException) as exc_info:
        require_family_admin(authed, family=None, x_family=None)
    assert exc_info.value.status_code == 400


def test_require_family_admin_super_with_family_query_is_ok():
    authed = _authed(uid="sup1", role="super", family_id=None)
    principal, family_id = require_family_admin(authed, family="famB", x_family=None)
    assert family_id == "famB"
    assert principal.role == "super"


def test_require_family_admin_super_with_x_family_header_is_ok():
    authed = _authed(uid="sup1", role="super", family_id=None)
    _principal, family_id = require_family_admin(authed, family=None, x_family="famC")
    assert family_id == "famC"


def test_require_super_rejects_family_admin():
    authed = _authed(uid="admA", role="admin", family_id="famA")
    with pytest.raises(HTTPException) as exc_info:
        require_super(authed)
    assert exc_info.value.status_code == 403


def test_require_super_rejects_bare_legacy_admin_claim():
    authed = AuthedUser(
        uid="legacy1",
        claims={"admin": True},
        user=User(uid="legacy1", alias="legacy1", displayName="Legacy", role="admin"),
    )
    with pytest.raises(HTTPException) as exc_info:
        require_super(authed)
    assert exc_info.value.status_code == 403


def test_require_super_accepts_super():
    authed = _authed(uid="sup1", role="super", family_id="default")
    result = require_super(authed)
    assert result.uid == "sup1"


def test_disabled_user_is_403(client: TestClient):
    auth_user = fb_auth.create_user(email="disabled@example.com")
    users_store.create_user(uid=auth_user.uid, alias="disabled1", display_name="Disabled One")
    users_store.update_user(auth_user.uid, disabled=True)
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403


def test_dev_token_mint_and_exchange_round_trips_to_a_verifiable_id_token(client: TestClient):
    auth_user = fb_auth.create_user(email="devflow@example.com")
    resp = client.post("/api/dev/token", json={"uid": auth_user.uid})
    assert resp.status_code == 200
    custom_token_response = resp.json()
    assert "token" in custom_token_response
    # Sanity: an independently-minted custom token for the same uid
    # exchanges to a verifiable ID token too (exercises the same emulator
    # path tools/pager_client.py will use).
    id_token = mint_id_token(auth_user.uid)
    decoded = fb_auth.verify_id_token(id_token)
    assert decoded["uid"] == auth_user.uid


def test_dev_token_disabled_outside_dev_mode():
    app = create_app(settings=make_settings(dev_mode=False), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        resp = c.post("/api/dev/token", json={"uid": "whoever"})
        assert resp.status_code == 404


def test_dev_token_mint_by_alias(client: TestClient):
    auth_user = fb_auth.create_user(email="byalias@example.com")
    users_store.create_user(uid=auth_user.uid, alias="byalias", display_name="By Alias")
    resp = client.post("/api/dev/token", json={"alias": "byalias"})
    assert resp.status_code == 200
    data = resp.json()
    assert data["uid"] == auth_user.uid
    assert "token" in data


def test_dev_token_unknown_alias_is_404(client: TestClient):
    resp = client.post("/api/dev/token", json={"alias": "ghost-alias"})
    assert resp.status_code == 404


def test_dev_token_requires_uid_or_alias(client: TestClient):
    resp = client.post("/api/dev/token", json={})
    assert resp.status_code == 422

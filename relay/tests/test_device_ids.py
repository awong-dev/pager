# ruff: noqa: F811
"""Relay-issued device ids and the label PATCH (owner decision 9 Oct 2026)."""

from __future__ import annotations

import re

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import devsetup
from app.store import devices as devices_store
from app.store import users as users_store
from tests.firebase_test_utils import auth_header
from tests.test_admin import (  # noqa: F401 -- fixtures
    admin_headers,
    broker,
    client,
    fake_emqx,
)
from tests.test_family_router import _make_family, _make_family_admin, _make_member

WIRE_ID = re.compile(r"^[a-z0-9][a-z0-9-]{2,23}$")

pytestmark = pytest.mark.real_device_ids


def _owner(client: TestClient, admin_headers: dict[str, str], alias: str = "own1") -> None:
    resp = client.post(
        "/api/admin/users",
        json={"alias": alias, "displayName": alias, "email": f"{alias}@example.com"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text


def _create(client: TestClient, headers: dict[str, str], body: dict) -> dict:
    resp = client.post("/api/admin/devices", json=body, headers=headers)
    assert resp.status_code == 200, resp.text
    return resp.json()


def test_generated_id_matches_wire_format_and_is_unique(client, admin_headers):
    _owner(client, admin_headers)
    ids = set()
    for _ in range(2):
        data = _create(client, admin_headers, {"ownerAlias": "own1", "label": "p"})
        did = data["device"]["id"]
        assert did.startswith("pgr-") and WIRE_ID.match(did)
        assert data["device"]["mqttUsername"] == did
        ids.add(did)
    assert len(ids) == 2


def test_supplied_device_id_is_ignored(client, admin_headers):
    _owner(client, admin_headers)
    data = _create(
        client, admin_headers, {"deviceId": "mine-1234", "ownerAlias": "own1", "label": "p"}
    )
    assert data["device"]["id"] != "mine-1234"
    assert devices_store.get_device("mine-1234") is None


def test_collision_retries_then_500(client, admin_headers, monkeypatch):
    from app.routers import admin as admin_router

    _owner(client, admin_headers)
    first = _create(client, admin_headers, {"ownerAlias": "own1", "label": "p"})["device"]["id"]
    seq = iter([first, first, "pgr-00000001"])
    monkeypatch.setattr(admin_router, "new_device_id", lambda: next(seq))
    data = _create(client, admin_headers, {"ownerAlias": "own1", "label": "q"})
    assert data["device"]["id"] == "pgr-00000001"

    monkeypatch.setattr(admin_router, "new_device_id", lambda: first)
    resp = client.post(
        "/api/admin/devices", json={"ownerAlias": "own1", "label": "r"}, headers=admin_headers
    )
    assert resp.status_code == 500


def test_admin_patch_label_and_rotate_bundle_carries_it(client, admin_headers, monkeypatch):
    _owner(client, admin_headers)
    did = _create(client, admin_headers, {"ownerAlias": "own1", "label": "old"})["device"]["id"]

    resp = client.patch(f"/api/admin/devices/{did}", json={"label": "  Kitchen  "}, headers=admin_headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["id"] == did
    assert resp.json()["label"] == "Kitchen"
    assert devices_store.get_device(did).label == "Kitchen"

    seen: list[str] = []
    real_issue = devsetup.issue

    def spy(*args, **kwargs):
        seen.append(kwargs["label"])
        return real_issue(*args, **kwargs)

    monkeypatch.setattr(devsetup, "issue", spy)
    resp = client.post(f"/api/admin/devices/{did}/rotate-credentials", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    assert seen == ["Kitchen"]


@pytest.mark.parametrize("label", ["", "   ", "x" * 33, "é" * 17])
def test_patch_label_rejects_bad_values(client, admin_headers, label):
    _owner(client, admin_headers)
    did = _create(client, admin_headers, {"ownerAlias": "own1", "label": "old"})["device"]["id"]
    resp = client.patch(f"/api/admin/devices/{did}", json={"label": label}, headers=admin_headers)
    assert resp.status_code == 422
    assert devices_store.get_device(did).label == "old"


def test_patch_label_accepts_32_bytes_and_404(client, admin_headers):
    _owner(client, admin_headers)
    did = _create(client, admin_headers, {"ownerAlias": "own1", "label": "old"})["device"]["id"]
    resp = client.patch(f"/api/admin/devices/{did}", json={"label": "é" * 16}, headers=admin_headers)
    assert resp.status_code == 200
    resp = client.patch("/api/admin/devices/nope-123", json={"label": "a"}, headers=admin_headers)
    assert resp.status_code == 404


def test_patch_label_requires_super(client, admin_headers):
    _owner(client, admin_headers)
    did = _create(client, admin_headers, {"ownerAlias": "own1", "label": "old"})["device"]["id"]
    fb_auth.create_user(uid="plain", email="plain@example.com")
    users_store.create_user(uid="plain", alias="plain", display_name="p", role="member")
    fb_auth.set_custom_user_claims("plain", {"role": "member"})
    resp = client.patch(f"/api/admin/devices/{did}", json={"label": "a"}, headers=auth_header("plain"))
    assert resp.status_code == 403


def test_family_patch_label(client):
    fam = _make_family("A")
    other = _make_family("B")
    fam_admin = _make_family_admin("fa", "fa", fam.id)
    other_admin = _make_family_admin("oa", "oa", other.id)
    member = _make_member("kid", "kid", fam.id)
    resp = client.post(
        "/api/family/devices",
        json={"deviceId": "ignored-1", "ownerAlias": "kid", "label": "old"},
        headers=fam_admin,
    )
    assert resp.status_code == 200, resp.text
    did = resp.json()["device"]["id"]
    assert did.startswith("pgr-") and WIRE_ID.match(did)

    resp = client.patch(f"/api/family/devices/{did}", json={"label": "New"}, headers=fam_admin)
    assert resp.status_code == 200, resp.text
    assert resp.json()["label"] == "New"
    assert devices_store.get_device(did).label == "New"

    for bad in ("", "x" * 33):
        resp = client.patch(f"/api/family/devices/{did}", json={"label": bad}, headers=fam_admin)
        assert resp.status_code == 422

    resp = client.patch(f"/api/family/devices/{did}", json={"label": "Z"}, headers=other_admin)
    assert resp.status_code == 403
    resp = client.patch(f"/api/family/devices/{did}", json={"label": "Z"}, headers=member)
    assert resp.status_code == 403
    assert devices_store.get_device(did).label == "New"

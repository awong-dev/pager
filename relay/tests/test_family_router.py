"""`/api/family/*` -- docs/FAMILIES_DESIGN.md §4, docs/FAMILIES_TASKS.md 1.3:
family-scoped member/device/group CRUD, gated on `app.auth.
require_family_admin` (an `admin` acting on their own family, or `super`
naming one via `?family=`)."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass, field

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import devices as devices_store
from app.store import families as families_store
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


@dataclass
class FakeEmqxAdmin:
    """Same duck-typed stand-in as `tests/test_admin.py`'s own
    `FakeEmqxAdmin` -- duplicated rather than imported so this file has no
    cross-test-module dependency (matches that file's own docstring
    reasoning)."""

    ensured_devices: list[tuple[str, str, str]] = field(default_factory=list)
    ensured_boot: list[tuple[str, str]] = field(default_factory=list)
    deleted: list[str] = field(default_factory=list)

    def ensure_device(self, username: str, password: str, device_id: str) -> str:
        self.ensured_devices.append((username, password, device_id))
        return "ok"

    def ensure_boot_user(self, bid: str, password: str) -> str:
        self.ensured_boot.append((bid, password))
        return "ok"

    def delete_user(self, username: str) -> str:
        self.deleted.append(username)
        return "ok"


@pytest.fixture
def fake_emqx() -> FakeEmqxAdmin:
    return FakeEmqxAdmin()


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def client(fake_emqx: FakeEmqxAdmin, broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        yield c


def _make_family(name: str = "Family") -> families_store.Family:
    return families_store.create_family(name=name, created_by="root-uid")


def _make_family_admin(uid: str, alias: str, family_id: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(
        uid=uid, alias=alias, display_name=alias, role="admin", family_id=family_id
    )
    fb_auth.set_custom_user_claims(uid, {"role": "admin", "fam": family_id})
    return auth_header(uid)


def _make_member(
    uid: str, alias: str, family_id: str | None, role: str = "member"
) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(
        uid=uid, alias=alias, display_name=alias, role=role, family_id=family_id
    )
    fb_auth.set_custom_user_claims(uid, {"role": role, "fam": family_id or ""})
    return auth_header(uid)


def _make_super(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias, role="super")
    fb_auth.set_custom_user_claims(uid, {"role": "super", "fam": ""})
    return auth_header(uid)


# ---------------------------------------------------------------------------
# GET /api/family, /members, POST /members, PATCH /members/{uid}
# ---------------------------------------------------------------------------


def test_get_family_returns_doc_plus_member_and_device_counts(
    client: TestClient, fake_emqx: FakeEmqxAdmin
):
    family = _make_family("The Wongs")
    admin_headers = _make_family_admin("fam1-admin", "fam1-admin", family.id)
    _make_member("fam1-kid", "fam1-kid", family.id)

    resp = client.post(
        "/api/family/devices",
        json={"deviceId": "pgr-fam1-1", "ownerAlias": "fam1-kid", "label": "kid pager"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    resp = client.get("/api/family", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["id"] == family.id
    assert data["name"] == "The Wongs"
    assert data["memberCount"] == 2  # admin + kid
    assert data["deviceCount"] == 1


def test_list_members_scoped_to_own_family(client: TestClient):
    family_a = _make_family("A")
    family_b = _make_family("B")
    admin_a = _make_family_admin("fam2-admin-a", "fam2-admin-a", family_a.id)
    _make_member("fam2-kid-a", "fam2-kid-a", family_a.id)
    _make_member("fam2-kid-b", "fam2-kid-b", family_b.id)

    resp = client.get("/api/family/members", headers=admin_a)
    assert resp.status_code == 200, resp.text
    uids = {m["uid"] for m in resp.json()["members"]}
    assert uids == {"fam2-admin-a", "fam2-kid-a"}


def test_create_member_sets_family_id_and_claims(client: TestClient):
    family = _make_family("C")
    admin_headers = _make_family_admin("fam3-admin", "fam3-admin", family.id)

    resp = client.post(
        "/api/family/members",
        json={
            "alias": "fam3-newkid",
            "displayName": "New Kid",
            "email": "fam3-newkid@example.com",
            "role": "member",
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["familyId"] == family.id
    assert data["role"] == "member"

    refreshed = fb_auth.get_user(data["uid"])
    assert refreshed.custom_claims == {"role": "member", "fam": family.id}


def test_patch_member_updates_role_and_reissues_claims(client: TestClient):
    family = _make_family("D")
    admin_headers = _make_family_admin("fam4-admin", "fam4-admin", family.id)
    _make_member("fam4-kid", "fam4-kid", family.id)

    resp = client.patch(
        "/api/family/members/fam4-kid",
        json={"role": "admin", "displayName": "Promoted Kid"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["role"] == "admin"
    assert data["displayName"] == "Promoted Kid"

    refreshed = fb_auth.get_user("fam4-kid")
    assert refreshed.custom_claims == {"role": "admin", "fam": family.id}


def test_patch_member_404_for_uid_outside_family(client: TestClient):
    family_a = _make_family("E")
    family_b = _make_family("F")
    admin_a = _make_family_admin("fam5-admin-a", "fam5-admin-a", family_a.id)
    _make_member("fam5-kid-b", "fam5-kid-b", family_b.id)

    resp = client.patch(
        "/api/family/members/fam5-kid-b", json={"displayName": "x"}, headers=admin_a
    )
    assert resp.status_code == 404


def test_member_role_cannot_reach_family_routes(client: TestClient):
    family = _make_family("G")
    member_headers = _make_member("fam6-member", "fam6-member", family.id)

    resp = client.get("/api/family/members", headers=member_headers)
    assert resp.status_code == 403


def test_admin_naming_another_family_is_403(client: TestClient):
    family_a = _make_family("H")
    family_b = _make_family("I")
    admin_a = _make_family_admin("fam7-admin-a", "fam7-admin-a", family_a.id)

    resp = client.get(f"/api/family/members?family={family_b.id}", headers=admin_a)
    assert resp.status_code == 403


def test_super_requires_family_query_param(client: TestClient):
    super_headers = _make_super("fam8-super", "fam8-super")
    resp = client.get("/api/family/members", headers=super_headers)
    assert resp.status_code == 400


def test_super_with_family_query_can_act(client: TestClient):
    family = _make_family("J")
    _make_member("fam9-kid", "fam9-kid", family.id)
    super_headers = _make_super("fam9-super", "fam9-super")

    resp = client.get(f"/api/family/members?family={family.id}", headers=super_headers)
    assert resp.status_code == 200, resp.text
    uids = {m["uid"] for m in resp.json()["members"]}
    assert "fam9-kid" in uids


# ---------------------------------------------------------------------------
# devices
# ---------------------------------------------------------------------------


def test_list_devices_scoped_to_own_family(client: TestClient):
    family_a = _make_family("K")
    family_b = _make_family("L")
    admin_a = _make_family_admin("fam10-admin-a", "fam10-admin-a", family_a.id)
    _make_member("fam10-kid-a", "fam10-kid-a", family_a.id)
    _make_member("fam10-kid-b", "fam10-kid-b", family_b.id)
    client.post(
        "/api/family/devices",
        json={"deviceId": "pgr-fam10-a", "ownerAlias": "fam10-kid-a", "label": "a"},
        headers=admin_a,
    )
    admin_b = _make_family_admin("fam10-admin-b", "fam10-admin-b", family_b.id)
    client.post(
        "/api/family/devices",
        json={"deviceId": "pgr-fam10-b", "ownerAlias": "fam10-kid-b", "label": "b"},
        headers=admin_b,
    )

    resp = client.get("/api/family/devices", headers=admin_a)
    assert resp.status_code == 200, resp.text
    ids = {d["id"] for d in resp.json()}
    assert ids == {"pgr-fam10-a"}


def test_create_device_for_out_of_family_owner_is_403(client: TestClient):
    family_a = _make_family("M")
    family_b = _make_family("N")
    admin_a = _make_family_admin("fam11-admin-a", "fam11-admin-a", family_a.id)
    _make_member("fam11-kid-b", "fam11-kid-b", family_b.id)

    resp = client.post(
        "/api/family/devices",
        json={"deviceId": "pgr-fam11-1", "ownerAlias": "fam11-kid-b", "label": "x"},
        headers=admin_a,
    )
    assert resp.status_code == 403
    assert devices_store.get_device("pgr-fam11-1") is None


def test_device_ops_on_out_of_family_device_are_403(
    client: TestClient, fake_emqx: FakeEmqxAdmin
):
    family_a = _make_family("O")
    family_b = _make_family("P")
    admin_a = _make_family_admin("fam12-admin-a", "fam12-admin-a", family_a.id)
    admin_b = _make_family_admin("fam12-admin-b", "fam12-admin-b", family_b.id)
    _make_member("fam12-kid-b", "fam12-kid-b", family_b.id)
    client.post(
        "/api/family/devices",
        json={"deviceId": "pgr-fam12-1", "ownerAlias": "fam12-kid-b", "label": "x"},
        headers=admin_b,
    )

    assert (
        client.post(
            "/api/family/devices/pgr-fam12-1/rotate-credentials", headers=admin_a
        ).status_code
        == 403
    )
    assert (
        client.post("/api/family/devices/pgr-fam12-1/revoke", headers=admin_a).status_code == 403
    )
    assert (
        client.post(
            "/api/family/devices/pgr-fam12-1/cfg", json={"lock": {}}, headers=admin_a
        ).status_code
        == 403
    )
    assert (
        client.post(
            "/api/family/devices/pgr-fam12-1/ca", json={"action": "unpin"}, headers=admin_a
        ).status_code
        == 403
    )
    assert client.delete("/api/family/devices/pgr-fam12-1", headers=admin_a).status_code == 403
    # None of the refused calls touched the device.
    assert devices_store.get_device("pgr-fam12-1") is not None


def test_device_ops_on_own_family_device_succeed(client: TestClient, fake_emqx: FakeEmqxAdmin):
    family = _make_family("Q")
    admin_headers = _make_family_admin("fam13-admin", "fam13-admin", family.id)
    _make_member("fam13-kid", "fam13-kid", family.id)
    client.post(
        "/api/family/devices",
        json={"deviceId": "pgr-fam13-1", "ownerAlias": "fam13-kid", "label": "x"},
        headers=admin_headers,
    )

    resp = client.post(
        "/api/family/devices/pgr-fam13-1/rotate-credentials", headers=admin_headers
    )
    assert resp.status_code == 200, resp.text

    resp = client.post(
        "/api/family/devices/pgr-fam13-1/cfg", json={"lock": {"auto": 5}}, headers=admin_headers
    )
    assert resp.status_code == 200, resp.text

    resp = client.delete("/api/family/devices/pgr-fam13-1", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    assert devices_store.get_device("pgr-fam13-1") is None


# ---------------------------------------------------------------------------
# groups
# ---------------------------------------------------------------------------


def test_create_group_requires_family_admin(client: TestClient):
    family = _make_family("R")
    member_headers = _make_member("fam14-mom", "fam14-mom", family.id)
    _make_member("fam14-kid", "fam14-kid", family.id)

    resp = client.post(
        "/api/family/groups",
        json={"name": "Family", "alias": "fam14-fam", "memberUids": ["fam14-mom", "fam14-kid"]},
        headers=member_headers,
    )
    assert resp.status_code == 403


def test_create_group_success_creates_allow_edges(client: TestClient, broker: FakeBrokerClient):
    family = _make_family("S")
    admin_headers = _make_family_admin("fam15-admin", "fam15-admin", family.id)
    _make_member("fam15-kid1", "fam15-kid1", family.id)
    _make_member("fam15-kid2", "fam15-kid2", family.id)

    resp = client.post(
        "/api/family/groups",
        json={
            "name": "Family",
            "alias": "fam15-fam",
            "memberUids": ["fam15-kid1", "fam15-kid2"],
        },
        headers=admin_headers,
    )
    assert resp.status_code == 201, resp.text
    body = resp.json()
    assert body["alias"] == "fam15-fam"
    assert allow_store.is_message_allowed("fam15-kid1", "fam15-kid2")
    assert allow_store.is_message_allowed("fam15-kid2", "fam15-kid1")


def test_create_group_with_out_of_family_non_peer_is_403(client: TestClient):
    family_a = _make_family("T")
    family_b = _make_family("U")
    admin_a = _make_family_admin("fam16-admin-a", "fam16-admin-a", family_a.id)
    _make_member("fam16-kid-a", "fam16-kid-a", family_a.id)
    _make_member("fam16-kid-b", "fam16-kid-b", family_b.id)

    resp = client.post(
        "/api/family/groups",
        json={
            "name": "Family",
            "alias": "fam16-fam",
            "memberUids": ["fam16-kid-a", "fam16-kid-b"],
        },
        headers=admin_a,
    )
    assert resp.status_code == 403


def test_create_group_with_out_of_family_edge_peer_succeeds(client: TestClient):
    family_a = _make_family("V")
    family_b = _make_family("W")
    admin_a = _make_family_admin("fam17-admin-a", "fam17-admin-a", family_a.id)
    _make_member("fam17-kid-a", "fam17-kid-a", family_a.id)
    _make_member("fam17-kid-b", "fam17-kid-b", family_b.id)
    # A pre-existing message edge with the creator (fam17-admin-a) makes the
    # out-of-family peer eligible.
    allow_store.set_edge("fam17-admin-a", "fam17-kid-b", message=True, locate=False)

    resp = client.post(
        "/api/family/groups",
        json={
            "name": "Family",
            "alias": "fam17-fam",
            "memberUids": ["fam17-admin-a", "fam17-kid-a", "fam17-kid-b"],
        },
        headers=admin_a,
    )
    assert resp.status_code == 201, resp.text

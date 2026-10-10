"""`/api/family/*` -- docs/FAMILIES_DESIGN.md §4, docs/FAMILIES_TASKS.md 1.3:
family-scoped member/device/group CRUD, gated on `app.auth.
require_family_admin` (an `admin` acting on their own family, or `super`
naming one via `?family=`)."""

from __future__ import annotations

import json
from collections.abc import Iterator
from dataclasses import dataclass, field

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import devcfg
from app.config import Settings
from app.db.firestore import get_db
from app.main import create_app
from app.store import allow as allow_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header
from tests.test_firmware import (  # noqa: F401 -- fixture
    BASE_URL,
    ID_NEW,
    ID_OLD,
    INDEX_URL,
    index_server,
)


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


def _make_pager_device(device_id: str, owner_uid: str, family_id: str) -> None:
    """`auth_mode="password"` (unlike `POST /api/family/devices`'s real
    setup-code flow, which defaults to `"hmac"`) so `devcfg`'s pushes stay
    plain JSON -- this file's assertions decode `broker.published[i].payload`
    with `json.loads`, same convention `tests/test_devcfg.py`'s own
    `_make_pager_device` uses."""
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
        family_id=family_id,
    )


def _make_super(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias, role="super")
    fb_auth.set_custom_user_claims(uid, {"role": "super", "fam": ""})
    return auth_header(uid)


def _create_contact(client: TestClient, headers: dict[str, str], phone: str, name: str) -> dict:
    resp = client.post(
        "/api/family/contacts", json={"phone": phone, "name": name}, headers=headers
    )
    assert resp.status_code == 201, resp.text
    return resp.json()


def _set_policy(uid: str, out: str, in_: str = "people") -> None:
    get_db().collection("users").document(uid).update({"policy": {"out": out, "in": in_}})


def _sms_pushes(broker: FakeBrokerClient, device_id: str) -> list[dict]:
    """Every `cfg.sms` push published to `device_id`, oldest first."""
    decoded = [json.loads(m.payload) for m in broker.published if m.topic == f"pager/{device_id}/down"]
    return [d for d in decoded if d["kind"] == "cfg" and "sms" in d["cfg"]]


def _put_contacts(
    client: TestClient, headers: dict[str, str], member: str, contacts: list[dict]
):
    return client.put(
        f"/api/family/members/{member}/approved",
        json={"people": [], "contacts": contacts},
        headers=headers,
    )


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
    # docs/FAMILIES_TASKS.md 3.2 addition (b): group creation now writes
    # message-only edges (never `locate`) -- the cross-family pair here
    # (fam17-kid-a, fam17-kid-b) would otherwise trip
    # `allow_store.check_locate_family` the moment a `/approved`-style
    # caller enforces it, so this is the case that actually needed create
    # to stop requesting `locate=True`.
    cross_family_edge = allow_store.get_edge("fam17-kid-a", "fam17-kid-b")
    assert cross_family_edge is not None
    assert cross_family_edge.message is True
    assert cross_family_edge.locate is False


# ---------------------------------------------------------------------------
# GET /api/directory -- docs/FAMILIES_TASKS.md 3.2 addition (a). Lives here
# (not `tests/test_me.py`, which another agent is concurrently editing)
# even though the route itself is `app/routers/me.py`.
# ---------------------------------------------------------------------------


def test_directory_admin_sees_edges_any_family_member_approved(client: TestClient):
    family = _make_family("Y")
    admin_headers = _make_family_admin("fam18-admin", "fam18-admin", family.id)
    _make_member("fam18-kid", "fam18-kid", family.id)
    users_store.create_user(
        uid="fam18-ext", alias="15551234567", display_name="Pizza Place", kind="external"
    )
    # The *member*, not the admin, holds the edge -- the admin has none of
    # their own to this external.
    allow_store.set_edge("fam18-kid", "fam18-ext", message=True, locate=False)

    resp = client.get("/api/directory", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    uids = {e["uid"] for e in resp.json()["entries"]}
    assert "fam18-ext" in uids


def test_directory_super_sees_family_edges_only_with_family_param(client: TestClient):
    family = _make_family("Z")
    _make_family_admin("fam19-admin", "fam19-admin", family.id)
    _make_member("fam19-kid", "fam19-kid", family.id)
    users_store.create_user(
        uid="fam19-ext", alias="15559876543", display_name="Grandma", kind="external"
    )
    allow_store.set_edge("fam19-kid", "fam19-ext", message=True, locate=False)
    super_headers = _make_super("fam19-super", "fam19-super")

    resp_without = client.get("/api/directory", headers=super_headers)
    assert resp_without.status_code == 200, resp_without.text
    assert "fam19-ext" not in {e["uid"] for e in resp_without.json()["entries"]}

    resp_with = client.get(
        "/api/directory", params={"family": family.id}, headers=super_headers
    )
    assert resp_with.status_code == 200, resp_with.text
    assert "fam19-ext" in {e["uid"] for e in resp_with.json()["entries"]}


# ---------------------------------------------------------------------------
# app/store/externals.py -- docs/FAMILIES_TASKS.md 3.2.
# ---------------------------------------------------------------------------


def test_externals_get_or_create_is_idempotent_by_phone():
    a = externals_store.get_or_create("famA", "+1 555 222 3333", "Pizza Place")
    b = externals_store.get_or_create("famA", "5552223333", "Pizza Place (again)")
    assert a.uid == b.uid
    assert (a.uid, a.alias) == externals_store.contact_ids("famA", "+15552223333")
    assert a.familyId is None
    assert a.ownerFamilyId == "famA"
    assert a.phone == "+15552223333"
    assert a.kind == "external"
    assert a.displayName == "Pizza Place"
    # One `sms` backend (no web client) and no phoneIndex doc.
    kinds = [
        snap.to_dict()["kind"]
        for snap in get_db().collection("users").document(a.uid).collection("backends").stream()
    ]
    assert kinds == ["sms"]
    assert list(get_db().collection("phoneIndex").stream()) == []


def test_externals_get_or_create_default_region_us():
    ext = externals_store.get_or_create("famA", "2065550100", "Local")
    assert ext.phone == "+12065550100"


# ---------------------------------------------------------------------------
# PUT /api/family/members/{uid}/approved -- docs/FAMILIES_TASKS.md 3.2.
# ---------------------------------------------------------------------------


def test_approved_put_contacts_derives_capped_sms_contacts(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("Approved1")
    admin_headers = _make_family_admin("fam20-admin", "fam20-admin", family.id)
    _make_member("fam20-kid", "fam20-kid", family.id)
    _make_pager_device("pgr-fam20-1", "fam20-kid", family.id)
    _set_policy("fam20-kid", "people_sms")

    contacts = [
        _create_contact(client, admin_headers, f"+1206555010{i}", f"n{i}") for i in range(9)
    ]
    # `people_sms` implies nothing: the pager lists only what is picked.
    assert devices_store.get_device("pgr-fam20-1").smsContacts == []

    resp = _put_contacts(client, admin_headers, "fam20-kid", [{"uid": c["uid"]} for c in contacts])
    assert resp.status_code == 200, resp.text
    assert len(resp.json()["contacts"]) == 9

    device = devices_store.get_device("pgr-fam20-1")
    assert len(device.smsContacts) == 8
    names = [c.name for c in device.smsContacts]
    assert names == sorted(names)

    for c in contacts:
        assert allow_store.is_message_allowed("fam20-kid", c["uid"])

    sms_pushes = _sms_pushes(broker, "pgr-fam20-1")
    assert sms_pushes, "no cfg.sms push found"
    assert len(sms_pushes[-1]["cfg"]["sms"]) == 8

    # A contact never reaches `c[]`.
    decoded = [json.loads(m.payload) for m in broker.published]
    for book_push in (d for d in decoded if d["kind"] == "book"):
        assert not any(c["t"] == "sms" for c in book_push["c"])


def test_approved_put_removing_a_contact_removes_edge_and_device_contact(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("Approved2")
    admin_headers = _make_family_admin("fam21-admin", "fam21-admin", family.id)
    _make_member("fam21-kid", "fam21-kid", family.id)
    _make_pager_device("pgr-fam21-1", "fam21-kid", family.id)
    mom = _create_contact(client, admin_headers, "+12065550100", "Mom")
    dad = _create_contact(client, admin_headers, "+12065550101", "Dad")

    resp = _put_contacts(client, admin_headers, "fam21-kid", [{"uid": mom["uid"]}, {"uid": dad["uid"]}])
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam21-kid", dad["uid"]) is not None

    resp = _put_contacts(client, admin_headers, "fam21-kid", [{"uid": mom["uid"]}])
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam21-kid", dad["uid"]) is None

    device = devices_store.get_device("pgr-fam21-1")
    assert [c.phone for c in device.smsContacts] == ["+12065550100"]


def test_approved_put_locate_refused_cross_family(client: TestClient):
    family_a = _make_family("AppA")
    family_b = _make_family("AppB")
    admin_a = _make_family_admin("fam22-admin-a", "fam22-admin-a", family_a.id)
    _make_member("fam22-kid-a", "fam22-kid-a", family_a.id)
    _make_member("fam22-kid-b", "fam22-kid-b", family_b.id)

    resp = client.put(
        "/api/family/members/fam22-kid-a/approved",
        json={
            "people": [{"alias": "fam22-kid-b", "message": True, "locate": True}],
            "contacts": [],
        },
        headers=admin_a,
    )
    assert resp.status_code == 400, resp.text
    assert "locate_cross_family" in resp.text
    assert allow_store.get_edge("fam22-kid-a", "fam22-kid-b") is None


def test_put_approved_contacts_unknown_uid_is_404_and_writes_nothing(client: TestClient):
    family = _make_family("AppU")
    admin = _make_family_admin("fam22u-admin", "fam22u-admin", family.id)
    _make_member("fam22u-kid", "fam22u-kid", family.id)
    mom = _create_contact(client, admin, "+12065550100", "Mom")

    resp = _put_contacts(
        client, admin, "fam22u-kid", [{"uid": mom["uid"]}, {"uid": "x_does_not_exist"}]
    )
    assert resp.status_code == 404, resp.text
    assert resp.json()["detail"] == "no such contact: x_does_not_exist"
    # Validation happens before any write: not even the valid one landed.
    assert allow_store.get_edge("fam22u-kid", mom["uid"]) is None


def test_put_approved_contacts_other_family_uid_is_404(client: TestClient):
    fam_a = _make_family("AppOA")
    fam_b = _make_family("AppOB")
    admin_a = _make_family_admin("fam22o-admin-a", "fam22o-admin-a", fam_a.id)
    admin_b = _make_family_admin("fam22o-admin-b", "fam22o-admin-b", fam_b.id)
    _make_member("fam22o-kid-a", "fam22o-kid-a", fam_a.id)
    theirs = _create_contact(client, admin_b, "+12065550100", "Theirs")

    resp = _put_contacts(client, admin_a, "fam22o-kid-a", [{"uid": theirs["uid"]}])
    assert resp.status_code == 404, resp.text
    assert allow_store.get_edge("fam22o-kid-a", theirs["uid"]) is None
    # A person's uid is not a contact either.
    resp = _put_contacts(client, admin_a, "fam22o-kid-a", [{"uid": "fam22o-admin-a"}])
    assert resp.status_code == 404, resp.text


def test_put_approved_numbers_field_is_422(client: TestClient):
    family = _make_family("AppN")
    admin = _make_family_admin("fam22n-admin", "fam22n-admin", family.id)
    _make_member("fam22n-kid", "fam22n-kid", family.id)

    resp = client.put(
        "/api/family/members/fam22n-kid/approved",
        json={"people": [], "numbers": [{"phone": "+12065550100", "name": "Mom"}]},
        headers=admin,
    )
    assert resp.status_code == 422, resp.text
    assert externals_store.list_family_contacts(family.id) == []


def test_put_approved_never_creates_an_external(client: TestClient):
    family = _make_family("AppNC")
    admin = _make_family_admin("fam22c-admin", "fam22c-admin", family.id)
    _make_member("fam22c-kid", "fam22c-kid", family.id)
    mom = _create_contact(client, admin, "+12065550100", "Mom")

    resp = _put_contacts(client, admin, "fam22c-kid", [{"uid": mom["uid"]}])
    assert resp.status_code == 200, resp.text
    assert [u.uid for u in externals_store.list_family_contacts(family.id)] == [mom["uid"]]
    assert resp.json()["contacts"] == [{"uid": mom["uid"], "message": True}]


def test_put_approved_contact_denied_writes_message_false_and_hides_implied(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("AppD")
    admin = _make_family_admin("fam22d-admin", "fam22d-admin", family.id)
    _make_member("fam22d-kid", "fam22d-kid", family.id)
    _make_pager_device("pgr-fam22d", "fam22d-kid", family.id)
    _set_policy("fam22d-kid", "open")
    mom = _create_contact(client, admin, "+12065550100", "Mom")
    assert [c.phone for c in devices_store.get_device("pgr-fam22d").smsContacts] == ["+12065550100"]

    resp = _put_contacts(client, admin, "fam22d-kid", [{"uid": mom["uid"], "message": False}])
    assert resp.status_code == 200, resp.text
    edge = allow_store.get_edge("fam22d-kid", mom["uid"])
    assert edge is not None and edge.message is False
    assert devices_store.get_device("pgr-fam22d").smsContacts == []
    assert _sms_pushes(broker, "pgr-fam22d")[-1]["cfg"]["sms"] == []

    listed = {c["uid"]: c for c in client.get("/api/family/contacts", headers=admin).json()}
    assert "fam22d-kid" not in listed[mom["uid"]]["impliedFor"]
    assert "fam22d-kid" not in listed[mom["uid"]]["approvedFor"]


def test_get_approved_returns_people_and_contacts(client: TestClient):
    family = _make_family("AppG")
    admin = _make_family_admin("fam22g-admin", "fam22g-admin", family.id)
    _make_member("fam22g-kid", "fam22g-kid", family.id)
    _make_member("fam22g-sib", "fam22g-sib", family.id)
    mom = _create_contact(client, admin, "+12065550100", "Mom")
    _create_contact(client, admin, "+12065550101", "Unpicked")
    resp = client.put(
        "/api/family/members/fam22g-kid/approved",
        json={
            "people": [{"alias": "fam22g-sib", "message": True, "locate": False}],
            "contacts": [{"uid": mom["uid"]}],
        },
        headers=admin,
    )
    assert resp.status_code == 200, resp.text

    resp = client.get("/api/family/members/fam22g-kid/approved", headers=admin)
    assert resp.status_code == 200, resp.text
    assert resp.json() == {
        "people": [{"alias": "fam22g-sib", "message": True, "locate": False}],
        "contacts": [{"uid": mom["uid"], "message": True}],
    }

    resp = client.get("/api/family/members/nobody/approved", headers=admin)
    assert resp.status_code == 404


def _put_people(client: TestClient, headers: dict[str, str], member: str, people: list[dict]):
    return client.put(
        f"/api/family/members/{member}/approved",
        json={"people": people, "contacts": []},
        headers=headers,
    )


def _person(alias: str, locate: bool = False) -> dict:
    return {"alias": alias, "message": True, "locate": locate}


def _get_approved(client: TestClient, headers: dict[str, str], member: str) -> dict:
    resp = client.get(f"/api/family/members/{member}/approved", headers=headers)
    assert resp.status_code == 200, resp.text
    return resp.json()


def test_approved_put_unlisted_person_edge_is_removed(client: TestClient):
    family = _make_family("AppR1")
    admin = _make_family_admin("fam23-admin", "fam23-admin", family.id)
    _make_member("fam23-kid", "fam23-kid", family.id)
    _make_member("fam23-sib1", "fam23-sib1", family.id)
    _make_member("fam23-sib2", "fam23-sib2", family.id)

    resp = _put_people(
        client, admin, "fam23-kid", [_person("fam23-sib1"), _person("fam23-sib2")]
    )
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam23-kid", "fam23-sib2") is not None

    resp = _put_people(client, admin, "fam23-kid", [_person("fam23-sib1")])
    assert resp.status_code == 200, resp.text
    assert [p["alias"] for p in _get_approved(client, admin, "fam23-kid")["people"]] == [
        "fam23-sib1"
    ]
    assert allow_store.get_edge("fam23-kid", "fam23-sib2") is None


def test_approved_put_check_one_uncheck_other_in_one_save(client: TestClient):
    family = _make_family("AppR2")
    admin = _make_family_admin("fam24-admin", "fam24-admin", family.id)
    _make_member("fam24-kid", "fam24-kid", family.id)
    _make_member("fam24-sib1", "fam24-sib1", family.id)
    _make_member("fam24-sib2", "fam24-sib2", family.id)
    assert _put_people(client, admin, "fam24-kid", [_person("fam24-sib1")]).status_code == 200

    resp = _put_people(client, admin, "fam24-kid", [_person("fam24-sib2")])
    assert resp.status_code == 200, resp.text
    assert [p["alias"] for p in resp.json()["people"]] == ["fam24-sib2"]
    assert resp.json() == _get_approved(client, admin, "fam24-kid")
    assert allow_store.get_edge("fam24-kid", "fam24-sib1") is None


def test_approved_put_removing_person_bumps_book(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("AppR3")
    admin = _make_family_admin("fam25-admin", "fam25-admin", family.id)
    _make_member("fam25-kid", "fam25-kid", family.id)
    _make_member("fam25-sib", "fam25-sib", family.id)
    _make_pager_device("pgr-fam25-1", "fam25-kid", family.id)
    assert _put_people(client, admin, "fam25-kid", [_person("fam25-sib")]).status_code == 200

    before = devcfg.get_book_version("pgr-fam25-1")
    broker.published.clear()
    resp = _put_people(client, admin, "fam25-kid", [])
    assert resp.status_code == 200, resp.text
    assert devcfg.get_book_version("pgr-fam25-1") == before + 1
    pushes = [json.loads(m.payload) for m in broker.published]
    assert any(d["kind"] == "book" for d in pushes)


def test_approved_put_leaves_peer_reverse_edge_alone(client: TestClient):
    family = _make_family("AppR4")
    admin = _make_family_admin("fam26-admin", "fam26-admin", family.id)
    _make_member("fam26-kid", "fam26-kid", family.id)
    _make_member("fam26-sib1", "fam26-sib1", family.id)
    allow_store.set_edge("fam26-sib1", "fam26-kid", message=True, locate=False)
    assert _put_people(client, admin, "fam26-kid", [_person("fam26-sib1")]).status_code == 200

    resp = _put_people(client, admin, "fam26-kid", [])
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam26-kid", "fam26-sib1") is None
    assert allow_store.get_edge("fam26-sib1", "fam26-kid") is not None


def test_approved_put_cross_family_person_edge_kept_if_listed_else_deleted(
    client: TestClient,
):
    fam_a = _make_family("AppR5A")
    fam_b = _make_family("AppR5B")
    admin_a = _make_family_admin("fam27-admin-a", "fam27-admin-a", fam_a.id)
    _make_member("fam27-kid-a", "fam27-kid-a", fam_a.id)
    _make_member("fam27-kid-b", "fam27-kid-b", fam_b.id)
    allow_store.set_edge("fam27-kid-a", "fam27-kid-b", message=True, locate=False)

    resp = _put_people(client, admin_a, "fam27-kid-a", [_person("fam27-kid-b")])
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam27-kid-a", "fam27-kid-b") is not None
    assert [p["alias"] for p in _get_approved(client, admin_a, "fam27-kid-a")["people"]] == [
        "fam27-kid-b"
    ]

    resp = _put_people(client, admin_a, "fam27-kid-a", [])
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam27-kid-a", "fam27-kid-b") is None


# ---------------------------------------------------------------------------
# GET/POST/PATCH /api/family/contacts -- docs/FAMILIES_TASKS.md 3.2.
# ---------------------------------------------------------------------------


def test_family_contacts_lists_only_family_externals_create_and_rename(
    client: TestClient,
):
    family_a = _make_family("ContA")
    family_b = _make_family("ContB")
    admin_a = _make_family_admin("fam23-admin-a", "fam23-admin-a", family_a.id)
    _make_member("fam23-kid-a", "fam23-kid-a", family_a.id)
    admin_b = _make_family_admin("fam23-admin-b", "fam23-admin-b", family_b.id)
    _make_member("fam23-kid-b", "fam23-kid-b", family_b.id)

    grandma = _create_contact(client, admin_a, "+12065550200", "Grandma")
    uncle = _create_contact(client, admin_b, "+12065550300", "Uncle")
    assert _put_contacts(client, admin_a, "fam23-kid-a", [{"uid": grandma["uid"]}]).status_code == 200
    assert _put_contacts(client, admin_b, "fam23-kid-b", [{"uid": uncle["uid"]}]).status_code == 200

    resp = client.get("/api/family/contacts", headers=admin_a)
    assert resp.status_code == 200, resp.text
    assert {c["displayName"] for c in resp.json()} == {"Grandma"}

    resp = client.post(
        "/api/family/contacts",
        json={"phone": "+12065550400", "name": "Pizza"},
        headers=admin_a,
    )
    assert resp.status_code == 201, resp.text
    pizza_uid = resp.json()["uid"]
    assert resp.json()["approvedFor"] == []
    assert resp.json()["phone"] == "+12065550400"

    resp = client.get("/api/family/contacts", headers=admin_a)
    # docs/CONTACT_REQ_DESIGN.md decision 7: the list is the family's own
    # contacts, edge or not; another family's contacts never appear.
    assert {c["displayName"] for c in resp.json()} == {"Grandma", "Pizza"}

    resp = client.patch(
        f"/api/family/contacts/{pizza_uid}", json={"name": "Pizza Place"}, headers=admin_a
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["approvedFor"] == []

    # Family B cannot rename family A's contact.
    resp = client.patch(
        f"/api/family/contacts/{pizza_uid}", json={"name": "Mine"}, headers=admin_b
    )
    assert resp.status_code == 404, resp.text

    resp = client.patch(
        f"/api/family/contacts/{grandma['uid']}", json={"name": "Grandma W."}, headers=admin_a
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["displayName"] == "Grandma W."
    assert resp.json()["approvedFor"] == ["fam23-kid-a"]


def test_create_contact_pushes_cfg_sms_to_open_members_only(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("ImpOpen")
    admin = _make_family_admin("fam23a-admin", "fam23a-admin", family.id)  # policy open
    _make_member("fam23a-kid", "fam23a-kid", family.id)  # policy people
    _make_pager_device("pgr-fam23a-admin", "fam23a-admin", family.id)
    _make_pager_device("pgr-fam23a-kid", "fam23a-kid", family.id)

    _create_contact(client, admin, "+12065550100", "Grandma")

    assert [c.phone for c in devices_store.get_device("pgr-fam23a-admin").smsContacts] == [
        "+12065550100"
    ]
    assert devices_store.get_device("pgr-fam23a-kid").smsContacts == []
    assert _sms_pushes(broker, "pgr-fam23a-admin")[-1]["cfg"]["sms"] == [
        {"n": "Grandma", "p": "+12065550100"}
    ]
    assert _sms_pushes(broker, "pgr-fam23a-kid")[-1]["cfg"]["sms"] == []


def test_create_contact_duplicate_name_is_409(client: TestClient):
    family = _make_family("Dup")
    admin = _make_family_admin("fam23b-admin", "fam23b-admin", family.id)
    _create_contact(client, admin, "+12065550100", "Grandma")

    resp = client.post(
        "/api/family/contacts", json={"phone": "+12065550101", "name": "Grandma"}, headers=admin
    )
    assert resp.status_code == 409, resp.text
    assert resp.json()["detail"] == 'a contact named "Grandma" already exists'
    assert externals_store.get_family_contact(family.id, "+12065550101") is None
    assert len(externals_store.list_family_contacts(family.id)) == 1
    # Another family may use the same name.
    other = _make_family("DupOther")
    other_admin = _make_family_admin("fam23b-other", "fam23b-other", other.id)
    _create_contact(client, other_admin, "+12065550102", "Grandma")


def test_create_contact_name_collision_after_truncation_and_case_is_409(client: TestClient):
    family = _make_family("Trunc")
    admin = _make_family_admin("fam23c-admin", "fam23c-admin", family.id)
    # The pager shows 16 code points and matches by name: these two are the
    # same name on the device.
    _create_contact(client, admin, "+12065550100", "Grandma Josephine Smith")
    resp = client.post(
        "/api/family/contacts",
        json={"phone": "+12065550101", "name": "GRANDMA JOSEPHINE Jones"},
        headers=admin,
    )
    assert resp.status_code == 409, resp.text
    assert len(externals_store.list_family_contacts(family.id)) == 1


def test_create_contact_same_number_twice_is_idempotent_and_keeps_name(client: TestClient):
    family = _make_family("Idem")
    admin = _make_family_admin("fam23d-admin", "fam23d-admin", family.id)
    first = _create_contact(client, admin, "+12065550100", "Grandma")
    resp = client.post(
        "/api/family/contacts", json={"phone": "206-555-0100", "name": "Nana"}, headers=admin
    )
    assert resp.status_code == 201, resp.text
    assert resp.json()["uid"] == first["uid"]
    assert resp.json()["displayName"] == "Grandma"
    assert len(externals_store.list_family_contacts(family.id)) == 1
    # The unused name was not reserved by the idempotent call.
    _create_contact(client, admin, "+12065550101", "Nana")


def test_rename_contact_to_taken_name_is_409(client: TestClient):
    family = _make_family("RenTaken")
    admin = _make_family_admin("fam23e-admin", "fam23e-admin", family.id)
    _create_contact(client, admin, "+12065550100", "Grandma")
    pizza = _create_contact(client, admin, "+12065550101", "Pizza")

    resp = client.patch(
        f"/api/family/contacts/{pizza['uid']}", json={"name": "grandma"}, headers=admin
    )
    assert resp.status_code == 409, resp.text
    assert users_store.get_user(pizza["uid"]).displayName == "Pizza"
    # Case-only change of its own name is fine.
    resp = client.patch(
        f"/api/family/contacts/{pizza['uid']}", json={"name": "PIZZA"}, headers=admin
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["displayName"] == "PIZZA"


def test_rename_contact_frees_old_name_for_reuse(client: TestClient):
    family = _make_family("RenFree")
    admin = _make_family_admin("fam23f-admin", "fam23f-admin", family.id)
    gma = _create_contact(client, admin, "+12065550100", "Grandma")
    resp = client.patch(
        f"/api/family/contacts/{gma['uid']}", json={"name": "Nana"}, headers=admin
    )
    assert resp.status_code == 200, resp.text
    _create_contact(client, admin, "+12065550101", "Grandma")


def test_rename_contact_repushes_cfg_sms_to_explicit_and_implied_holders(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("RenPush")
    admin = _make_family_admin("fam23g-admin", "fam23g-admin", family.id)  # implied (open)
    _make_member("fam23g-kid", "fam23g-kid", family.id)  # explicit
    _make_member("fam23g-sib", "fam23g-sib", family.id)  # neither
    for owner in ("admin", "kid", "sib"):
        _make_pager_device(f"pgr-fam23g-{owner}", f"fam23g-{owner}", family.id)
    gma = _create_contact(client, admin, "+12065550100", "Grandma")
    assert _put_contacts(client, admin, "fam23g-kid", [{"uid": gma["uid"]}]).status_code == 200
    broker.clear()

    resp = client.patch(
        f"/api/family/contacts/{gma['uid']}", json={"name": "Nana"}, headers=admin
    )
    assert resp.status_code == 200, resp.text
    assert [c.name for c in devices_store.get_device("pgr-fam23g-admin").smsContacts] == ["Nana"]
    assert [c.name for c in devices_store.get_device("pgr-fam23g-kid").smsContacts] == ["Nana"]
    assert devices_store.get_device("pgr-fam23g-sib").smsContacts == []
    assert _sms_pushes(broker, "pgr-fam23g-admin")[-1]["cfg"]["sms"][0]["n"] == "Nana"
    assert _sms_pushes(broker, "pgr-fam23g-kid")[-1]["cfg"]["sms"][0]["n"] == "Nana"


def test_delete_contact_removes_edges_frees_name_and_rederives(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("Del")
    admin = _make_family_admin("fam23h-admin", "fam23h-admin", family.id)
    _make_member("fam23h-kid", "fam23h-kid", family.id)
    _make_pager_device("pgr-fam23h-kid", "fam23h-kid", family.id)
    _make_pager_device("pgr-fam23h-admin", "fam23h-admin", family.id)
    gma = _create_contact(client, admin, "+12065550100", "Grandma")
    assert _put_contacts(client, admin, "fam23h-kid", [{"uid": gma["uid"]}]).status_code == 200
    assert len(devices_store.get_device("pgr-fam23h-kid").smsContacts) == 1
    assert len(devices_store.get_device("pgr-fam23h-admin").smsContacts) == 1

    resp = client.delete(f"/api/family/contacts/{gma['uid']}", headers=admin)
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"ok": True}

    assert users_store.get_user(gma["uid"]) is None
    assert users_store.get_uid_for_alias(gma["alias"]) is None
    assert allow_store.get_edge("fam23h-kid", gma["uid"]) is None
    assert client.get("/api/family/contacts", headers=admin).json() == []
    assert devices_store.get_device("pgr-fam23h-kid").smsContacts == []
    assert devices_store.get_device("pgr-fam23h-admin").smsContacts == []
    assert _sms_pushes(broker, "pgr-fam23h-kid")[-1]["cfg"]["sms"] == []
    # The name is free again.
    _create_contact(client, admin, "+12065550101", "Grandma")

    resp = client.delete(f"/api/family/contacts/{gma['uid']}", headers=admin)
    assert resp.status_code == 404


def test_delete_contact_of_other_family_is_404(client: TestClient):
    fam_a = _make_family("DelA")
    fam_b = _make_family("DelB")
    admin_a = _make_family_admin("fam23i-admin-a", "fam23i-admin-a", fam_a.id)
    admin_b = _make_family_admin("fam23i-admin-b", "fam23i-admin-b", fam_b.id)
    gma = _create_contact(client, admin_a, "+12065550100", "Grandma")

    resp = client.delete(f"/api/family/contacts/{gma['uid']}", headers=admin_b)
    assert resp.status_code == 404, resp.text
    assert users_store.get_user(gma["uid"]) is not None
    # A person is not a contact.
    resp = client.delete("/api/family/contacts/fam23i-admin-a", headers=admin_a)
    assert resp.status_code == 404, resp.text


def test_list_contacts_reports_implied_for_open_members(client: TestClient):
    family = _make_family("ImpList")
    admin = _make_family_admin("fam23j-admin", "fam23j-admin", family.id)  # open
    _make_member("fam23j-kid", "fam23j-kid", family.id)  # people
    _make_member("fam23j-open", "fam23j-open", family.id)
    _make_member("fam23j-denied", "fam23j-denied", family.id)
    _make_member("fam23j-anysms", "fam23j-anysms", family.id)
    _set_policy("fam23j-open", "open")
    _set_policy("fam23j-denied", "open")
    _set_policy("fam23j-anysms", "any_sms")
    gma = _create_contact(client, admin, "+12065550100", "Grandma")
    allow_store.set_edge("fam23j-denied", gma["uid"], message=False, locate=False)
    assert _put_contacts(client, admin, "fam23j-kid", [{"uid": gma["uid"]}]).status_code == 200

    (listed,) = client.get("/api/family/contacts", headers=admin).json()
    assert listed["approvedFor"] == ["fam23j-kid"]
    assert listed["impliedFor"] == ["fam23j-admin", "fam23j-anysms", "fam23j-open"]


def test_policy_change_to_open_adds_family_contacts_to_cfg_sms(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("PolOpen")
    admin = _make_family_admin("fam23k-admin", "fam23k-admin", family.id)
    _make_member("fam23k-kid", "fam23k-kid", family.id)
    _make_pager_device("pgr-fam23k", "fam23k-kid", family.id)
    _create_contact(client, admin, "+12065550100", "Grandma")
    assert devices_store.get_device("pgr-fam23k").smsContacts == []

    resp = client.patch(
        "/api/family/members/fam23k-kid",
        json={"policy": {"out": "open", "in": "people"}},
        headers=admin,
    )
    assert resp.status_code == 200, resp.text
    assert [c.name for c in devices_store.get_device("pgr-fam23k").smsContacts] == ["Grandma"]
    assert _sms_pushes(broker, "pgr-fam23k")[-1]["cfg"]["sms"] == [
        {"n": "Grandma", "p": "+12065550100"}
    ]


def test_policy_change_to_people_drops_implied_contacts_keeps_edges(client: TestClient):
    family = _make_family("PolPeople")
    admin = _make_family_admin("fam23l-admin", "fam23l-admin", family.id)
    _make_member("fam23l-kid", "fam23l-kid", family.id)
    _make_pager_device("pgr-fam23l", "fam23l-kid", family.id)
    _set_policy("fam23l-kid", "open")
    picked = _create_contact(client, admin, "+12065550100", "Aunt")
    _create_contact(client, admin, "+12065550101", "Bob")
    assert _put_contacts(client, admin, "fam23l-kid", [{"uid": picked["uid"]}]).status_code == 200
    assert [c.name for c in devices_store.get_device("pgr-fam23l").smsContacts] == ["Aunt", "Bob"]

    resp = client.patch(
        "/api/family/members/fam23l-kid",
        json={"policy": {"out": "people", "in": "people"}},
        headers=admin,
    )
    assert resp.status_code == 200, resp.text
    assert [c.name for c in devices_store.get_device("pgr-fam23l").smsContacts] == ["Aunt"]
    assert allow_store.is_message_allowed("fam23l-kid", picked["uid"])


def test_implied_cfg_sms_caps_at_eight_name_sorted(client: TestClient):
    family = _make_family("ImpCap")
    admin = _make_family_admin("fam23m-admin", "fam23m-admin", family.id)
    _make_pager_device("pgr-fam23m", "fam23m-admin", family.id)
    for i in reversed(range(10)):
        _create_contact(client, admin, f"+1206555010{i}", f"c{i}")

    names = [c.name for c in devices_store.get_device("pgr-fam23m").smsContacts]
    assert names == [f"c{i}" for i in range(8)]


# ---------------------------------------------------------------------------
# POST /api/conversations/{alias}/messages with a phone-number alias --
# docs/FAMILIES_TASKS.md 3.2.
# ---------------------------------------------------------------------------


def test_send_message_to_unknown_phone_number_is_404_and_creates_nothing(
    client: TestClient,
):
    family = _make_family("Msg1")
    admin_headers = _make_family_admin("fam24-admin", "fam24-admin", family.id)
    member_headers = _make_member("fam24-kid", "fam24-kid", family.id)
    resp = client.patch(
        "/api/family/members/fam24-kid",
        json={"policy": {"out": "open", "in": "people"}},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    resp = client.post(
        "/api/conversations/+12065550500/messages",
        json={"body": "hi"},
        headers=member_headers,
    )
    assert resp.status_code == 404, resp.text
    assert externals_store.list_family_contacts(family.id) == []
    assert externals_store.get_family_contact(family.id, "+12065550500") is None


def test_send_message_to_unapproved_phone_number_is_404_without_any_number_policy(
    client: TestClient,
):
    family = _make_family("Msg2")
    admin_headers = _make_family_admin("fam25-admin", "fam25-admin", family.id)
    member_headers = _make_member("fam25-kid", "fam25-kid", family.id)
    resp = client.patch(
        "/api/family/members/fam25-kid",
        json={"policy": {"out": "people_sms", "in": "people"}},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    resp = client.post(
        "/api/conversations/+12065550600/messages",
        json={"body": "hi"},
        headers=member_headers,
    )
    assert resp.status_code == 404, resp.text


# ---------------------------------------------------------------------------
# docs/CONTACT_REQ_DESIGN.md decisions 5, 6, 7
# ---------------------------------------------------------------------------


def test_create_member_normalises_phone_and_rejects_garbage(client: TestClient):
    family = _make_family("Phones")
    admin = _make_family_admin("fam30-admin", "fam30-admin", family.id)

    resp = client.post(
        "/api/family/members",
        json={"alias": "fam30-kid", "displayName": "Kid", "phone": "2065550100"},
        headers=admin,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["phone"] == "+12065550100"
    assert fb_auth.get_user(resp.json()["uid"]).phone_number == "+12065550100"

    resp = client.post(
        "/api/family/members",
        json={"alias": "fam30-bad", "displayName": "Bad", "phone": "abc"},
        headers=admin,
    )
    assert resp.status_code == 400
    assert resp.json()["detail"] == "phone must be a number like +12065550100"
    assert users_store.get_uid_for_alias("fam30-bad") is None


def test_family_admin_renames_own_family(client: TestClient):
    family = _make_family("Old Name")
    other = _make_family("Other")
    admin = _make_family_admin("fam31-admin", "fam31-admin", family.id)
    other_admin = _make_family_admin("fam31-other", "fam31-other", other.id)

    resp = client.patch("/api/family", json={"name": "  Wongs "}, headers=admin)
    assert resp.status_code == 200, resp.text
    assert resp.json()["name"] == "Wongs"
    assert resp.json()["memberCount"] == 1
    assert families_store.get_family(family.id).name == "Wongs"
    assert client.get("/api/family", headers=admin).json()["name"] == "Wongs"

    # Scope: another family's admin names their own family, never this one;
    # naming a different family explicitly is refused.
    resp = client.patch("/api/family", params={"family": family.id}, json={"name": "X"}, headers=other_admin)
    assert resp.status_code == 403, resp.text
    assert families_store.get_family(family.id).name == "Wongs"
    assert families_store.get_family(other.id).name == "Other"


@pytest.mark.parametrize("bad", ["", "   ", "x" * 41, "a\nb"])
def test_family_rename_rejects_bad_names(client: TestClient, bad: str):
    family = _make_family("Keep")
    admin = _make_family_admin("fam32-admin", "fam32-admin", family.id)
    resp = client.patch("/api/family", json={"name": bad}, headers=admin)
    assert resp.status_code == 400, resp.text
    assert families_store.get_family(family.id).name == "Keep"
    assert families_store.validate_family_name("x" * 40) == "x" * 40


def test_two_families_approving_one_number_get_their_own_names_and_sms_contacts(
    client: TestClient,
):
    fam_a = _make_family("SmsA")
    fam_b = _make_family("SmsB")
    admin_a = _make_family_admin("fam33-admin-a", "fam33-admin-a", fam_a.id)
    admin_b = _make_family_admin("fam33-admin-b", "fam33-admin-b", fam_b.id)
    _make_member("fam33-kid-a", "fam33-kid-a", fam_a.id)
    _make_member("fam33-kid-b", "fam33-kid-b", fam_b.id)
    _make_pager_device("pgr-fam33-a", "fam33-kid-a", fam_a.id)
    _make_pager_device("pgr-fam33-b", "fam33-kid-b", fam_b.id)

    for headers, kid, name in (
        (admin_a, "fam33-kid-a", "Grandma"),
        (admin_b, "fam33-kid-b", "Gran Jo"),
    ):
        contact = _create_contact(client, headers, "+12065550100", name)
        resp = _put_contacts(client, headers, kid, [{"uid": contact["uid"]}])
        assert resp.status_code == 200, resp.text

    contacts_a = client.get("/api/family/contacts", headers=admin_a).json()
    contacts_b = client.get("/api/family/contacts", headers=admin_b).json()
    assert [c["displayName"] for c in contacts_a] == ["Grandma"]
    assert [c["displayName"] for c in contacts_b] == ["Gran Jo"]
    assert contacts_a[0]["uid"] != contacts_b[0]["uid"]
    assert contacts_a[0]["phone"] == contacts_b[0]["phone"] == "+12065550100"

    dev_a = devices_store.get_device("pgr-fam33-a")
    dev_b = devices_store.get_device("pgr-fam33-b")
    assert [c.name for c in dev_a.smsContacts] == ["Grandma"]
    assert [c.name for c in dev_b.smsContacts] == ["Gran Jo"]

    # Rename in A re-derives A's kid's list only.
    resp = client.patch(
        f"/api/family/contacts/{contacts_a[0]['uid']}", json={"name": "Nana"}, headers=admin_a
    )
    assert resp.status_code == 200, resp.text
    assert [c.name for c in devices_store.get_device("pgr-fam33-a").smsContacts] == ["Nana"]
    assert [c.name for c in devices_store.get_device("pgr-fam33-b").smsContacts] == ["Gran Jo"]


def test_two_families_keep_separate_contacts_for_one_number():
    fam_a = families_store.create_family(name="A", created_by="s")
    fam_b = families_store.create_family(name="B", created_by="s")
    a = externals_store.get_or_create(fam_a.id, "+12065550100", "Grandma")
    b = externals_store.get_or_create(fam_b.id, "206-555-0100", "Gran Jo")
    assert a.uid != b.uid and a.alias != b.alias
    assert (a.displayName, b.displayName) == ("Grandma", "Gran Jo")
    assert (a.phone, b.phone) == ("+12065550100", "+12065550100")
    assert externals_store.get_family_contact(fam_a.id, "+12065550100").uid == a.uid
    assert externals_store.get_family_contact(fam_b.id, "+12065550100").uid == b.uid
    assert [u.uid for u in externals_store.list_family_contacts(fam_a.id)] == [a.uid]

    db = get_db()
    for uid in (a.uid, b.uid):
        kinds = [s.to_dict()["kind"] for s in db.collection("users").document(uid).collection("backends").stream()]
        assert kinds == ["sms"]
    assert list(db.collection("phoneIndex").stream()) == []


def test_approve_open_sms_unknown_alert_creates_the_contact_and_rederives(client: TestClient):
    from app import alerts as alerts_module

    family = _make_family("Unk")
    admin = _make_family_admin("fam34-admin", "fam34-admin", family.id)
    _make_member("fam34-kid", "fam34-kid", family.id)
    _make_pager_device("pgr-fam34", "fam34-kid", family.id)
    alert_id = alerts_module.sms_unknown(family.id, "+12065550100", "fam34-kid", "hello")

    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve", json={"name": "Auntie"}, headers=admin
    )
    assert resp.status_code == 200, resp.text
    ext = externals_store.get_family_contact(family.id, "+12065550100")
    assert ext is not None and ext.displayName == "Auntie"
    assert allow_store.is_message_allowed("fam34-kid", ext.uid)
    assert [c.phone for c in devices_store.get_device("pgr-fam34").smsContacts] == ["+12065550100"]


def test_start_chat_by_number_without_own_sms_number_is_403_no_sms_number(
    client: TestClient,
):
    family = _make_family("Start")
    admin = _make_family_admin("fam35-admin", "fam35-admin", family.id)
    kid = _make_member("fam35-kid", "fam35-kid", family.id)
    ext = externals_store.get_or_create(family.id, "+12065550100", "Grandma")
    allow_store.set_edge("fam35-kid", ext.uid, message=True, locate=False)
    resp = client.patch(
        "/api/family/members/fam35-kid",
        json={"policy": {"out": "people_sms", "in": "people"}},
        headers=admin,
    )
    assert resp.status_code == 200, resp.text

    resp = client.post("/api/conversations/+12065550100/messages", json={"body": "hi"}, headers=kid)
    assert resp.status_code == 403, resp.text
    assert resp.json()["detail"]["reason"] == "no_sms_number"
    assert resp.json()["detail"]["message"] == "You have no SMS number; ask your family admin."
    assert list(get_db().collection("messages").stream()) == []
    assert list(get_db().collection("conversations").stream()) == []

    # An unknown number is still a 404.
    resp = client.post("/api/conversations/+12065550999/messages", json={"body": "hi"}, headers=kid)
    assert resp.status_code == 404, resp.text


def test_create_group_with_external_member_is_400(client: TestClient):
    family = _make_family("GrpExt")
    admin = _make_family_admin("fam36-admin", "fam36-admin", family.id)
    _make_member("fam36-kid", "fam36-kid", family.id)
    ext = externals_store.get_or_create(family.id, "+12065550100", "Grandma")

    resp = client.post(
        "/api/family/groups",
        json={
            "name": "G",
            "alias": "fam36grp",
            "memberUids": ["fam36-admin", "fam36-kid", ext.uid],
        },
        headers=admin,
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["detail"] == "an SMS contact cannot join a group"
    assert users_store.get_uid_for_alias("fam36grp") is None


# ---------------------------------------------------------------------------
# OTA + APN (docs/OTA_DESIGN.md D10, owner decision 9 Oct 2026)
# ---------------------------------------------------------------------------


@pytest.fixture
def ota_client(
    fake_emqx: FakeEmqxAdmin, broker: FakeBrokerClient, index_server: dict  # noqa: F811
) -> Iterator[TestClient]:
    settings = make_settings(fw_index_url=INDEX_URL, fw_bucket_base=BASE_URL)
    app = create_app(settings=settings, broker_client=broker)
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        yield c


def _ota_family_device(device_id: str, family_id: str, *, img: str | None = None) -> None:
    _make_pager_device(device_id, "owner-ota", family_id)
    devices_store.update_status(device_id, state="online", img=img, otaCap=1)


def test_family_admin_lists_firmware_for_in_family_device(ota_client: TestClient):
    fam, other = _make_family("OtaA"), _make_family("OtaB")
    admin = _make_family_admin("ota-adm", "ota-adm", fam.id)
    _ota_family_device("pgr-ota-in", fam.id, img=ID_OLD[:16])
    _ota_family_device("pgr-ota-out", other.id)

    r = ota_client.get("/api/family/firmware?device=pgr-ota-in", headers=admin)
    assert r.status_code == 200, r.text
    builds = r.json()["builds"]
    assert [b["id16"] for b in builds] == [ID_NEW[:16], ID_OLD[:16]]
    assert builds[0]["kind"] == "delta" and builds[1]["kind"] == "full"

    assert ota_client.get("/api/family/firmware?device=pgr-ota-out", headers=admin).status_code == 403
    assert ota_client.get("/api/family/firmware", headers=admin).status_code == 422


def test_family_admin_pushes_and_cancels_ota(ota_client: TestClient, broker: FakeBrokerClient):
    fam, other = _make_family("OtaC"), _make_family("OtaD")
    admin = _make_family_admin("ota-adm2", "ota-adm2", fam.id)
    member = _make_member("ota-mem", "ota-mem", fam.id)
    _ota_family_device("pgr-ota-in", fam.id, img="9" * 16)
    _ota_family_device("pgr-ota-out", other.id)
    body = {"target": ID_NEW[:16]}

    r = ota_client.post("/api/family/devices/pgr-ota-in/ota", json=body, headers=admin)
    assert r.status_code == 200, r.text
    assert r.json()["kind"] == "full"
    assert json.loads(broker.published[-1].payload)["cfg"]["ota"]["fmt"] == "full"
    job = devices_store.get_device("pgr-ota-in").otaJob
    assert job is not None and job.by_uid == "ota-adm2" and job.target16 == ID_NEW[:16]

    r = ota_client.post("/api/family/devices/pgr-ota-in/ota", json={"cancel": True}, headers=admin)
    assert r.status_code == 200 and r.json() == {"ok": True}
    assert devices_store.get_device("pgr-ota-in").otaJob is None

    assert ota_client.post("/api/family/devices/pgr-ota-out/ota", json=body, headers=admin).status_code == 403
    assert ota_client.post("/api/family/devices/pgr-ota-in/ota", json=body, headers=member).status_code == 403

    sup = _make_super("ota-sup", "ota-sup")
    r = ota_client.post(f"/api/family/devices/pgr-ota-in/ota?family={fam.id}", json=body, headers=sup)
    assert r.status_code == 200, r.text
    assert devices_store.get_device("pgr-ota-in").otaJob.by_uid == "ota-sup"


def test_family_admin_apn_in_and_out_of_family(client: TestClient):
    fam, other = _make_family("ApnA"), _make_family("ApnB")
    admin = _make_family_admin("apn-adm", "apn-adm", fam.id)
    _make_pager_device("pgr-apn-in", "o", fam.id)
    _make_pager_device("pgr-apn-out", "o", other.id)

    presets = client.get("/api/family/apn-presets", headers=admin)
    assert presets.status_code == 200 and presets.json()
    apn = presets.json()[0]["apn"]
    r = client.put("/api/family/devices/pgr-apn-in/apn", json={"apn": apn}, headers=admin)
    assert r.status_code == 200, r.text
    assert r.json() == {"apn": apn}
    assert devices_store.get_device("pgr-apn-in").apn == apn
    r = client.put("/api/family/devices/pgr-apn-out/apn", json={"apn": apn}, headers=admin)
    assert r.status_code == 403


# ---- contact_request alerts: approve an added entry; remove an added entry ----
# docs/BOOK_ADD_ANYONE_DESIGN.md D10, D15


def _added_setup(
    client: TestClient, policy: tuple[str, str] = ("people_sms", "people_sms")
) -> tuple[families_store.Family, dict[str, str], str]:
    """A family with an admin, a kid (relay number, a pager) and an external
    the kid added from the pager (marker, no edge). Returns the family, the
    admin's headers and the external's uid."""
    family = _make_family("AddedFam")
    admin = _make_family_admin("parent1", "parent1", family.id)
    _make_member("kid1", "kid1", family.id)
    get_db().collection("users").document("kid1").update(
        {"policy": {"out": policy[0], "in": policy[1]}}
    )
    users_store.set_sms_number("kid1", "+12065550999")
    _make_pager_device("pgr-add-1", "kid1", family.id)
    ext = externals_store.get_or_create(family.id, "+12065550100", "Grandma")
    get_db().collection("users").document("kid1").collection("book").document(ext.uid).set(
        {"added": True, "familyId": family.id}
    )
    return family, admin, ext.uid


def _raise_alert(family_id: str, ext_uid: str) -> str:
    from app import alerts as alerts_module

    owner = users_store.get_user("kid1")
    peer = users_store.get_user(ext_uid)
    assert alerts_module.approval_upsert(owner, peer) == "created"
    return f"cr_kid1_{ext_uid}"


def test_approve_added_writes_edge_and_bumps(client: TestClient, broker: FakeBrokerClient):
    from app.routing import Routing

    family, admin, ext_uid = _added_setup(client)
    alert_id = _raise_alert(family.id, ext_uid)
    bv = devcfg.get_book_version("pgr-add-1")
    peer = users_store.get_user(ext_uid)
    # Before approval the send is refused.
    refused = Routing(broker).send(
        sender_uid="kid1", recipient_alias=peer.alias, kind="text", body="hi",
        origin_backend_kind="pager",
    )
    assert [r.reason for r in refused.rejected] == ["not_allowed"]

    resp = client.post(f"/api/family/alerts/{alert_id}/approve", json={}, headers=admin)

    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"
    edge = allow_store.get_edge("kid1", ext_uid)
    assert edge is not None and edge.message is True and edge.locate is False
    # (the relay-number owner's `rederive_sms_contacts` bumps once more)
    assert devcfg.get_book_version("pgr-add-1") > bv
    assert any(json.loads(m.payload).get("kind") == "book" for m in broker.published)
    # A resend now passes the gate.
    sent = Routing(broker).send(
        sender_uid="kid1", recipient_alias=peer.alias, kind="text", body="hi again",
        origin_backend_kind="pager",
    )
    assert sent.rejected == []


def test_approve_keeps_existing_locate(client: TestClient):
    family, admin, ext_uid = _added_setup(client)
    allow_store.set_edge("kid1", ext_uid, message=False, locate=True)
    alert_id = _raise_alert(family.id, ext_uid)

    resp = client.post(f"/api/family/alerts/{alert_id}/approve", json={}, headers=admin)

    assert resp.status_code == 200, resp.text
    edge = allow_store.get_edge("kid1", ext_uid)
    assert edge is not None and edge.message is True and edge.locate is True


def test_approve_legacy_409(client: TestClient):
    from app.store import alerts as alerts_store
    from tests.firebase_test_utils import seed_legacy_contact_request

    family, admin, ext_uid = _added_setup(client)
    key = seed_legacy_contact_request(
        "pgr-add-1", "kid1", "u_old", "Old", "+12065550177", family_id=family.id
    )
    (alert,) = [a for a in alerts_store.list_alerts(family.id, "open") if a.contactRequestKey == key]

    resp = client.post(f"/api/family/alerts/{alert.id}/approve", json={}, headers=admin)

    assert resp.status_code == 409
    assert resp.json()["detail"] == "superseded; add again from the pager"
    assert alerts_store.get(family.id, alert.id).status == "open"
    assert allow_store.get_edge("kid1", ext_uid) is None


def test_approve_external_rederives_cfg_sms_for_open_siblings(client: TestClient, broker):
    """An approved external reaches every family member `sms_contacts_for`
    derives it for (here a modem-owner sibling with `open`)."""
    family, admin, ext_uid = _added_setup(client)
    _make_member("sib1", "sib1", family.id)
    get_db().collection("users").document("sib1").update(
        {"policy": {"out": "open", "in": "people"}}
    )
    _make_pager_device("pgr-add-sib", "sib1", family.id)
    alert_id = _raise_alert(family.id, ext_uid)

    resp = client.post(f"/api/family/alerts/{alert_id}/approve", json={}, headers=admin)

    assert resp.status_code == 200, resp.text
    assert [c.phone for c in devices_store.get_device("pgr-add-sib").smsContacts] == [
        "+12065550100"
    ]


def _marker(owner: str, peer: str) -> dict | None:
    snap = get_db().collection("users").document(owner).collection("book").document(peer).get()
    return snap.to_dict() if snap.exists else None


def test_book_entries_carry_added(client: TestClient):
    _family, admin, ext_uid = _added_setup(client)
    resp = client.get("/api/book", params={"uid": "kid1"}, headers=admin)
    assert resp.status_code == 200, resp.text
    (entry,) = [e for e in resp.json()["entries"] if e["uid"] == ext_uid]
    assert entry["added"] is True and entry["sendable"] is False


def test_remove_added_keeps_nick(client: TestClient, broker: FakeBrokerClient):
    family, admin, ext_uid = _added_setup(client)
    get_db().collection("users").document("kid1").collection("book").document(ext_uid).update(
        {"nick": "Gran"}
    )
    bv = devcfg.get_book_version("pgr-add-1")
    broker.clear()

    resp = client.delete(f"/api/book/kid1/added/{ext_uid}", headers=admin)

    assert resp.status_code == 200, resp.text
    marker = _marker("kid1", ext_uid)
    assert marker is not None and marker["nick"] == "Gran" and "added" not in marker
    assert devcfg.get_book_version("pgr-add-1") == bv + 1
    assert any(json.loads(m.payload).get("kind") == "book" for m in broker.published)
    assert ext_uid not in [e["uid"] for e in resp.json()["entries"]]
    # Edges and the family contact are untouched; a second remove is a 404.
    assert externals_store.get_family_contact(family.id, "+12065550100") is not None
    assert client.delete(f"/api/book/kid1/added/{ext_uid}", headers=admin).status_code == 404


def test_remove_added_without_nick_deletes_the_doc(client: TestClient):
    _family, admin, ext_uid = _added_setup(client)

    resp = client.delete(f"/api/book/kid1/added/{ext_uid}", headers=admin)

    assert resp.status_code == 200, resp.text
    assert _marker("kid1", ext_uid) is None


def test_remove_added_other_family_404(client: TestClient):
    _family, _admin, ext_uid = _added_setup(client)
    other = _make_family("OtherFam")
    stranger = _make_family_admin("parent2", "parent2", other.id)

    resp = client.delete(f"/api/book/kid1/added/{ext_uid}", headers=stranger)

    assert resp.status_code == 404
    assert _marker("kid1", ext_uid)["added"] is True

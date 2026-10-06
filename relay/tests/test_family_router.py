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

from app.config import Settings
from app.db.firestore import get_db
from app.main import create_app
from app.store import allow as allow_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import messages as messages_store
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


def test_externals_get_or_create_default_region_us():
    ext = externals_store.get_or_create("famA", "2065550100", "Local")
    assert ext.phone == "+12065550100"


# ---------------------------------------------------------------------------
# PUT /api/family/members/{uid}/approved -- docs/FAMILIES_TASKS.md 3.2.
# ---------------------------------------------------------------------------


def test_approved_put_creates_externals_and_derives_capped_sms_contacts(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("Approved1")
    admin_headers = _make_family_admin("fam20-admin", "fam20-admin", family.id)
    _make_member("fam20-kid", "fam20-kid", family.id)
    _make_pager_device("pgr-fam20-1", "fam20-kid", family.id)
    # Numbers are listed on the pager only when the policy lets the kid text
    # them (docs/ADDRESS_BOOK_DESIGN.md decision 2: sendable entries only).
    get_db().collection("users").document("fam20-kid").update(
        {"policy": {"out": "people_sms", "in": "people"}}
    )

    numbers = [{"phone": f"+1206555010{i}", "name": f"n{i}"} for i in range(9)]
    resp = client.put(
        "/api/family/members/fam20-kid/approved",
        json={"people": [], "numbers": numbers},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert len(resp.json()["numbers"]) == 9

    device = devices_store.get_device("pgr-fam20-1")
    assert len(device.smsContacts) == 8
    names = [c.name for c in device.smsContacts]
    assert names == sorted(names)

    for n in numbers:
        ext = externals_store.get_or_create(family.id, n["phone"], n["name"])
        assert allow_store.is_message_allowed("fam20-kid", ext.uid)

    decoded = [json.loads(m.payload) for m in broker.published]
    sms_pushes = [d for d in decoded if d["kind"] == "cfg" and "sms" in d["cfg"]]
    assert sms_pushes, "no cfg.sms push found"
    assert len(sms_pushes[-1]["cfg"]["sms"]) == 8

    book_pushes = [d for d in decoded if d["kind"] == "book"]
    assert book_pushes, "no book push found"
    contact_types = {c["t"] for c in book_pushes[-1]["c"]}
    assert "sms" in contact_types


def test_approved_put_removing_a_number_removes_edge_and_device_contact(
    client: TestClient, broker: FakeBrokerClient
):
    family = _make_family("Approved2")
    admin_headers = _make_family_admin("fam21-admin", "fam21-admin", family.id)
    _make_member("fam21-kid", "fam21-kid", family.id)
    _make_pager_device("pgr-fam21-1", "fam21-kid", family.id)

    resp = client.put(
        "/api/family/members/fam21-kid/approved",
        json={
            "people": [],
            "numbers": [
                {"phone": "+12065550100", "name": "Mom"},
                {"phone": "+12065550101", "name": "Dad"},
            ],
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    dad = externals_store.get_or_create(family.id, "+12065550101", "Dad")
    assert allow_store.get_edge("fam21-kid", dad.uid) is not None

    resp = client.put(
        "/api/family/members/fam21-kid/approved",
        json={"people": [], "numbers": [{"phone": "+12065550100", "name": "Mom"}]},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert allow_store.get_edge("fam21-kid", dad.uid) is None

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
            "numbers": [],
        },
        headers=admin_a,
    )
    assert resp.status_code == 400, resp.text
    assert "locate_cross_family" in resp.text
    assert allow_store.get_edge("fam22-kid-a", "fam22-kid-b") is None


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

    client.put(
        "/api/family/members/fam23-kid-a/approved",
        json={"people": [], "numbers": [{"phone": "+12065550200", "name": "Grandma"}]},
        headers=admin_a,
    )
    client.put(
        "/api/family/members/fam23-kid-b/approved",
        json={"people": [], "numbers": [{"phone": "+12065550300", "name": "Uncle"}]},
        headers=admin_b,
    )

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

    grandma = externals_store.get_or_create(family_a.id, "+12065550200", "Grandma")
    resp = client.patch(
        f"/api/family/contacts/{grandma.uid}", json={"name": "Grandma W."}, headers=admin_a
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["displayName"] == "Grandma W."
    assert resp.json()["approvedFor"] == ["fam23-kid-a"]


# ---------------------------------------------------------------------------
# POST /api/conversations/{alias}/messages with a phone-number alias --
# docs/FAMILIES_TASKS.md 3.2.
# ---------------------------------------------------------------------------


def test_send_message_to_phone_number_creates_external_under_any_number_policy(
    client: TestClient,
):
    family = _make_family("Msg1")
    admin_headers = _make_family_admin("fam24-admin", "fam24-admin", family.id)
    member_headers = _make_member("fam24-kid", "fam24-kid", family.id)
    resp = client.patch(
        "/api/family/members/fam24-kid",
        json={"policy": {"out": "any_sms", "in": "people"}},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    resp = client.post(
        "/api/conversations/+12065550500/messages",
        json={"body": "hi"},
        headers=member_headers,
    )
    assert resp.status_code == 201, resp.text
    msg = messages_store.get_message(resp.json()["id"])
    assert msg is not None
    ext = users_store.get_user(msg.recipientUid)
    assert ext is not None and ext.kind == "external"
    assert ext.phone == "+12065550500"
    assert ext.ownerFamilyId == family.id


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
        resp = client.put(
            f"/api/family/members/{kid}/approved",
            json={"people": [], "numbers": [{"phone": "+12065550100", "name": name}]},
            headers=headers,
        )
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


def test_approve_open_sms_unknown_alert_creates_the_contact_and_rederives(client: TestClient):
    from app import alerts as alerts_module

    family = _make_family("Unk")
    admin = _make_family_admin("fam34-admin", "fam34-admin", family.id)
    _make_member("fam34-kid", "fam34-kid", family.id)
    _make_pager_device("pgr-fam34", "fam34-kid", family.id)
    alert_id = alerts_module.sms_unknown(
        family.id, "+12065550100", "fam34-kid", "hello", held=False, open_unheld=True
    )

    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve", json={"name": "Auntie"}, headers=admin
    )
    assert resp.status_code == 200, resp.text
    ext = externals_store.get_family_contact(family.id, "+12065550100")
    assert ext is not None and ext.displayName == "Auntie"
    assert allow_store.is_message_allowed("fam34-kid", ext.uid)
    assert [c.phone for c in devices_store.get_device("pgr-fam34").smsContacts] == ["+12065550100"]


def test_start_chat_by_number_finds_the_familys_existing_contact_under_any_policy(
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
    assert resp.status_code == 201, resp.text
    msg = messages_store.get_message(resp.json()["id"])
    assert msg is not None and msg.recipientUid == ext.uid

    # An unknown number under people_sms is still a 404.
    resp = client.post("/api/conversations/+12065550999/messages", json={"body": "hi"}, headers=kid)
    assert resp.status_code == 404, resp.text

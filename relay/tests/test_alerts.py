"""`app/alerts.py`, `app/store/alerts.py`, `POST /api/family/alerts/*` --
docs/FAMILIES_DESIGN.md §6 "Alert creation", §3 alerts fields, §9 item 6;
docs/FAMILIES_TASKS.md 4.1."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import alerts as alerts_module
from app import jobs
from app.config import Settings
from app.db.firestore import get_db
from app.main import create_app
from app.routing import Routing
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.alerts import list_alerts
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


@pytest.fixture
def routing(broker: FakeBrokerClient) -> Routing:
    return Routing(broker)


def _make_family(name: str = "Family") -> families_store.Family:
    return families_store.create_family(name=name, created_by="root-uid")


def _make_family_admin(uid: str, alias: str, family_id: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(
        uid=uid, alias=alias, display_name=alias, role="admin", family_id=family_id
    )
    fb_auth.set_custom_user_claims(uid, {"role": "admin", "fam": family_id})
    return auth_header(uid)


def _make_member(uid: str, alias: str, family_id: str, role: str = "member") -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias, role=role, family_id=family_id)


def _set_policy(uid: str, *, out: str, in_: str) -> None:
    """Whitebox: direct Firestore write of `users/{uid}.policy` (`app/store/
    users.py`'s `update_user` has no `policy` parameter)."""
    get_db().collection("users").document(uid).update({"policy": {"out": out, "in": in_}})


def _make_pager_device(device_id: str, owner_uid: str) -> None:
    """A real `devices/{id}` doc -- `_approve_contact_impl`'s approve path
    (reused by `POST /api/family/alerts/{id}/approve`'s `contact_request`
    case) ends by bumping and pushing that device's book, which needs every
    `Device` field `bump_book_version`'s own merge-write doesn't supply."""
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )


# ---------------------------------------------------------------------------
# app/store/alerts.py -- list/decide
# ---------------------------------------------------------------------------


def _minimal_alert(status: str, preview: str) -> dict:
    """A full-shape alert dict (every §3 field present, most `None`) --
    `app/backends/webapp.py`'s `push_alert` indexes `subjectAlias` directly
    (`a['subjectAlias']`), so a dict missing it 500s; the three named
    writers (`sms_unknown`/`new_conversation`/`contact_request`) always
    build the full shape via `app/alerts.py`'s own `_base_alert`, so tests
    that call the low-level `alerts_module.create` directly need to match
    it rather than send a partial dict no real caller would."""
    return {
        "kind": "sms_unknown",
        "status": status,
        "subjectUid": None,
        "subjectAlias": "someone",
        "peerUid": None,
        "peerAlias": None,
        "peerPhone": None,
        "preview": preview,
        "heldBody": None,
        "convKey": None,
        "contactRequestKey": None,
        "decidedAt": None,
        "decidedBy": None,
    }


def test_list_alerts_open_filters_and_all_returns_every_status():
    family = _make_family("Store")
    alerts_module.create(family.id, _minimal_alert("open", "a"))
    alerts_module.create(family.id, _minimal_alert("handled", "b"))

    open_only = list_alerts(family.id, "open")
    assert len(open_only) == 1
    assert open_only[0].status == "open"

    everything = list_alerts(family.id, "all")
    assert len(everything) == 2


def test_decide_sets_status_and_decided_fields():
    family = _make_family("Decide")
    alert_id = alerts_module.create(family.id, _minimal_alert("open", "x"))

    updated = alerts_store.decide(family.id, alert_id, "dismissed", "admin1")
    assert updated.status == "dismissed"
    assert updated.decidedBy == "admin1"
    assert updated.decidedAt is not None


# ---------------------------------------------------------------------------
# app/routing.py -- new_conversation
# ---------------------------------------------------------------------------


def test_open_members_first_dm_to_a_stranger_writes_one_new_conversation_alert(routing: Routing):
    family = _make_family("Open")
    # `admin` role -> default policy `open`/`any` (docs/FAMILIES_DESIGN.md §2).
    users_store.create_user(uid="mom", alias="mom", display_name="Mom", family_id=family.id, role="admin")
    users_store.create_user(
        uid="stranger", alias="stranger", display_name="Stranger", role="admin"
    )

    result = routing.send(
        sender_uid="mom", recipient_alias="stranger", kind="text", body="hi", origin_backend_kind="webapp"
    )
    assert result.ok

    alerts = list_alerts(family.id, "all")
    assert len(alerts) == 1
    assert alerts[0].kind == "new_conversation"
    assert alerts[0].subjectAlias == "mom"
    assert alerts[0].peerAlias == "stranger"
    assert alerts[0].peerUid == "stranger"
    assert alerts[0].convKey == messages_store.conv_key("mom", "stranger")

    # A second message to the same peer: the conversation already exists,
    # so no second alert.
    result2 = routing.send(
        sender_uid="mom",
        recipient_alias="stranger",
        kind="text",
        body="hi again",
        origin_backend_kind="webapp",
    )
    assert result2.ok
    assert len(list_alerts(family.id, "all")) == 1


def test_send_with_an_existing_edge_writes_no_alert_even_if_open(routing: Routing):
    family = _make_family("EdgeOpen")
    users_store.create_user(uid="dad", alias="dad", display_name="Dad", family_id=family.id, role="admin")
    users_store.create_user(uid="pal", alias="pal", display_name="Pal", role="admin")
    allow_store.set_edge("dad", "pal", message=True, locate=False)

    result = routing.send(
        sender_uid="dad", recipient_alias="pal", kind="text", body="hi", origin_backend_kind="webapp"
    )
    assert result.ok
    assert list_alerts(family.id, "all") == []


# ---------------------------------------------------------------------------
# POST /api/family/alerts/{id}/approve -- sms_unknown
# ---------------------------------------------------------------------------


def test_approve_sms_unknown_creates_external_edge_and_rederives_sms_contacts(
    client: TestClient,
):
    family = _make_family("Approve")
    headers = _make_family_admin("admin1", "admin1", family.id)
    _make_member("kid1", "kid1", family.id)
    _set_policy("kid1", out="people", in_="people_sms")
    _make_pager_device("pgr-kid1", "kid1")

    alert_id = alerts_module.sms_unknown(family.id, "+19995551234", "kid1", "hi there")
    alert = alerts_store.get(family.id, alert_id)
    assert alert is not None and alert.status == "open" and alert.heldBody is None
    assert alert.peerUid is None and alert.peerPhone == "+19995551234"

    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve", json={"name": "Aunt Sue"}, headers=headers
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["alert"]["status"] == "handled"
    assert resp.json()["delivered"] == 0 and resp.json()["undelivered"] == 0

    ext_uid, _alias = externals_store.contact_ids(family.id, "+19995551234")
    assert users_store.get_user(ext_uid) is not None
    assert allow_store.is_message_allowed("kid1", ext_uid)

    # The relay sends nothing: no message was created for that pair.
    assert messages_store.list_thread(messages_store.conv_key("kid1", ext_uid)) == []
    assert list(get_db().collection("messages").stream()) == []
    device = devices_store.get_device("pgr-kid1")
    assert [(c.name, c.phone) for c in device.smsContacts] == [("Aunt Sue", "+19995551234")]


def test_approve_sms_unknown_rederives_whole_family(client: TestClient):
    family = _make_family("WholeFam")
    headers = _make_family_admin("admin1b", "admin1b", family.id)
    _make_member("kid1b", "kid1b", family.id)
    _make_member("sib1b", "sib1b", family.id)
    _set_policy("sib1b", out="open", in_="people")
    _make_pager_device("pgr-kid1b", "kid1b")
    _make_pager_device("pgr-sib1b", "sib1b")

    alert_id = alerts_module.sms_unknown(family.id, "+19995551235", "kid1b", "hello")
    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve", json={"name": "Aunt Sue"}, headers=headers
    )
    assert resp.status_code == 200, resp.text

    assert [c.name for c in devices_store.get_device("pgr-kid1b").smsContacts] == ["Aunt Sue"]
    # The open sibling holds it by implied approval, with no edge.
    assert [c.name for c in devices_store.get_device("pgr-sib1b").smsContacts] == ["Aunt Sue"]


def test_approve_sms_unknown_taken_name_is_409(client: TestClient):
    family = _make_family("TakenUnk")
    headers = _make_family_admin("admin1c", "admin1c", family.id)
    _make_member("kid1c", "kid1c", family.id)
    externals_store.get_or_create(family.id, "+19995551111", "Aunt Sue")
    alert_id = alerts_module.sms_unknown(family.id, "+19995551236", "kid1c", "hello")

    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve", json={"name": "aunt sue"}, headers=headers
    )
    assert resp.status_code == 409, resp.text
    alert = alerts_store.get(family.id, alert_id)
    assert alert is not None and alert.status == "open"
    assert externals_store.get_family_contact(family.id, "+19995551236") is None


def test_approve_sms_unknown_with_no_subject_uses_for_alias(client: TestClient):
    family = _make_family("ForAlias")
    headers = _make_family_admin("admin2", "admin2", family.id)
    _make_member("kid2", "kid2", family.id)
    _set_policy("kid2", out="people", in_="people_sms")

    alert_id = alerts_module.sms_unknown(family.id, "+19995554321", None, "who is this?")

    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve",
        json={"name": "Uncle Bob", "forAlias": "kid2"},
        headers=headers,
    )
    assert resp.status_code == 200, resp.text

    ext_uid, _alias = externals_store.contact_ids(family.id, "+19995554321")
    assert users_store.get_user(ext_uid) is not None
    assert allow_store.is_message_allowed("kid2", ext_uid)


# ---------------------------------------------------------------------------
# POST /api/family/alerts/{id}/approve -- new_conversation
# ---------------------------------------------------------------------------


def test_approve_new_conversation_adds_the_edge(client: TestClient, routing: Routing):
    family = _make_family("ApproveConv")
    headers = _make_family_admin("admin3", "admin3", family.id)
    users_store.create_user(uid="teen", alias="teen", display_name="Teen", family_id=family.id, role="admin")
    users_store.create_user(uid="friend", alias="friend", display_name="Friend", role="admin")

    result = routing.send(
        sender_uid="teen", recipient_alias="friend", kind="text", body="hi", origin_backend_kind="webapp"
    )
    assert result.ok
    alerts = list_alerts(family.id, "all")
    assert len(alerts) == 1
    alert_id = alerts[0].id

    assert not allow_store.is_message_allowed("teen", "friend")
    resp = client.post(f"/api/family/alerts/{alert_id}/approve", json={}, headers=headers)
    assert resp.status_code == 200, resp.text
    assert allow_store.is_message_allowed("teen", "friend")


# ---------------------------------------------------------------------------
# POST /api/family/alerts/{id}/block
# ---------------------------------------------------------------------------


def test_block_adds_number_to_family_and_marks_handled(client: TestClient):
    family = _make_family("Block")
    headers = _make_family_admin("admin4", "admin4", family.id)

    alert_id = alerts_module.sms_unknown(family.id, "+19995559999", None, "spam")

    resp = client.post(f"/api/family/alerts/{alert_id}/block", json={}, headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"

    updated_family = families_store.get_family(family.id)
    assert updated_family is not None
    assert "+19995559999" in updated_family.blockedNumbers


# ---------------------------------------------------------------------------
# POST /api/family/alerts/{id}/dismiss
# ---------------------------------------------------------------------------


def test_dismiss_marks_alert_dismissed(client: TestClient):
    family = _make_family("Dismiss")
    headers = _make_family_admin("admin5", "admin5", family.id)
    alert_id = alerts_module.sms_unknown(family.id, "+19995551111", None, "hi")

    resp = client.post(f"/api/family/alerts/{alert_id}/dismiss", json={}, headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "dismissed"


# ---------------------------------------------------------------------------
# app/alerts.py -- approval_upsert (docs/BOOK_ADD_ANYONE_DESIGN.md D8)
# ---------------------------------------------------------------------------


def _approval_pair(family_id: str):
    users_store.create_user(uid="owner1", alias="owner1", display_name="Owner", family_id=family_id)
    peer = externals_store.get_or_create(family_id, "+15551230000", "Friend")
    owner = users_store.get_user("owner1")
    assert owner is not None
    return owner, peer


def test_approval_upsert_creates_one_alert_with_a_fixed_id(monkeypatch):
    pushed: list[dict] = []
    monkeypatch.setattr(
        alerts_module, "_push_alert", lambda fid, alert, fcm_client=None: pushed.append(alert)
    )
    family = _make_family("ContactReq")
    owner, peer = _approval_pair(family.id)

    assert alerts_module.approval_upsert(owner, peer) == "created"
    assert alerts_module.approval_upsert(owner, peer) == "open"

    alerts = list_alerts(family.id, "all")
    assert [a.id for a in alerts] == [f"cr_owner1_{peer.uid}"]
    alert = alerts[0]
    assert alert.kind == "contact_request" and alert.status == "open"
    assert alert.contactRequestKey is None
    assert (alert.subjectUid, alert.peerUid, alert.peerPhone) == ("owner1", peer.uid, "+15551230000")
    assert alert.peerName == "Friend"
    assert len(pushed) == 1 and pushed[0]["id"] == alert.id


def test_approval_upsert_declined_then_reopened(monkeypatch):
    pushed: list[dict] = []
    monkeypatch.setattr(
        alerts_module, "_push_alert", lambda fid, alert, fcm_client=None: pushed.append(alert)
    )
    family = _make_family("ContactReq2")
    owner, peer = _approval_pair(family.id)
    alert_id = f"cr_owner1_{peer.uid}"
    alerts_module.approval_upsert(owner, peer)
    alerts_store.decide(family.id, alert_id, "dismissed", "parent")

    assert alerts_module.approval_upsert(owner, peer) == "declined"
    assert alerts_store.get(family.id, alert_id).status == "dismissed"

    get_db().collection("families").document(family.id).collection("alerts").document(
        alert_id
    ).update({"decidedAt": datetime.now(UTC) - timedelta(hours=25)})
    assert alerts_module.approval_upsert(owner, peer) == "reopened"
    reopened = alerts_store.get(family.id, alert_id)
    assert reopened.status == "open" and reopened.decidedAt is None
    assert len(pushed) == 2 and len(list_alerts(family.id, "all")) == 1


def test_approve_contact_request_alert_grants_the_edge_only(client: TestClient):
    """docs/BOOK_ADD_ANYONE_DESIGN.md D10: the entry already exists; approving
    writes the owner's edge and nothing else (no person, no new contact)."""
    family = _make_family("ContactApprove")
    headers = _make_family_admin("admin6", "admin6", family.id)
    owner, peer = _approval_pair(family.id)
    alerts_module.approval_upsert(owner, peer)
    alert_id = f"cr_owner1_{peer.uid}"

    resp = client.post(
        f"/api/family/alerts/{alert_id}/approve",
        json={"mode": "create", "alias": "newpal"},
        headers=headers,
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["detail"] == "new people are added under Family > People"

    resp = client.post(f"/api/family/alerts/{alert_id}/approve", json={}, headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"

    assert users_store.get_uid_for_alias("newpal") is None
    assert allow_store.is_message_allowed("owner1", peer.uid)
    assert not allow_store.is_message_allowed(peer.uid, "owner1")
    assert [u.uid for u in externals_store.list_family_contacts(family.id)] == [peer.uid]
    raw_kinds = [
        snap.to_dict()["kind"]
        for snap in get_db().collection("users").document(peer.uid).collection("backends").stream()
    ]
    assert raw_kinds == ["sms"]


# ---------------------------------------------------------------------------
# app/jobs.py -- sweep() removes old handled/dismissed alerts only
# ---------------------------------------------------------------------------


def _backdate_alert(family_id: str, alert_id: str, days_ago: int) -> None:
    get_db().collection("families").document(family_id).collection("alerts").document(
        alert_id
    ).update({"ts": datetime.now(UTC) - timedelta(days=days_ago)})


def test_sweep_removes_only_old_handled_or_dismissed_alerts():
    family = _make_family("Sweep")

    old_handled = alerts_module.create(family.id, _minimal_alert("handled", "old handled"))
    old_dismissed = alerts_module.create(family.id, _minimal_alert("dismissed", "old dismissed"))
    old_open = alerts_module.create(family.id, _minimal_alert("open", "old open"))
    young_handled = alerts_module.create(family.id, _minimal_alert("handled", "young handled"))

    _backdate_alert(family.id, old_handled, 100)
    _backdate_alert(family.id, old_dismissed, 100)
    _backdate_alert(family.id, old_open, 100)
    _backdate_alert(family.id, young_handled, 5)

    jobs.sweep()

    remaining_ids = {a.id for a in list_alerts(family.id, "all")}
    assert remaining_ids == {old_open, young_handled}

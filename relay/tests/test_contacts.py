"""`kind:"contact_req"` ingest handling (docs/PROTOCOL.md §3.2, §4.2) and the
contact-request approve/block flow (docs/DEVICE_PLAN.md §4.3,
docs/FAMILIES_DESIGN.md §10 item 8) -- docs/DEVICE_TASKS.md S4.1,
docs/FAMILIES_TASKS.md 5.1.

Ingest-level tests exercise `app.ingest.Ingest.handle_up` directly, the same
style `tests/test_ingest.py` uses. The approve/block flow used to be
admin-only (`POST /api/admin/contacts/{key}/{approve|reject}`); task 5.1
deleted that router in favour of `POST /api/family/alerts/{id}/
{approve|block}` (`app/routers/family.py`, task 4.1) -- those tests go
through the real FastAPI app (`tests/test_admin.py`'s style) so the rate
limiting / `require_family_admin` dependency are exercised too, same as
before, just against the new route.
"""

from __future__ import annotations

import json
import time
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import devauth
from app.config import Settings
from app.db.firestore import get_db
from app.ingest import Ingest, classify_ph
from app.main import create_app
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import users as users_store
from tests.conftest import up_topic
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

_HMAC_KEY = b"k" * 32


# ---------------------------------------------------------------------------
# shared helpers
# ---------------------------------------------------------------------------


def _make_user(uid: str, alias: str, *, family_id: str | None = None) -> None:
    """`family_id` defaults to `None` for the ingest-level tests below,
    which never need one; the family-scoped alerts-route tests further down
    pass one so `app/alerts.py`'s `contact_request` (fired by
    `contacts_store.create_request`) has a family to raise the alert in --
    a device owner with no `familyId` gets no alert at all."""
    users_store.create_user(uid=uid, alias=alias, display_name=alias, family_id=family_id)


def _make_pager_device(device_id: str, owner_uid: str) -> None:
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def _make_hmac_pager_device(device_id: str, owner_uid: str, *, key: bytes = _HMAC_KEY) -> bytes:
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="hmac",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )
    device_secrets_store.create(device_id, hmac_key=key, mqtt_password_hash="y")
    return key


def _ingest() -> tuple[Ingest, FakeBrokerClient]:
    broker = FakeBrokerClient()
    return Ingest(broker), broker


def contact_req_payload(
    req_id: str, name: str, *, ph: str | None = None, ts: int | None = None
) -> bytes:
    ts = ts if ts is not None else int(time.time())
    obj: dict[str, object] = {
        "v": 1,
        "id": req_id,
        "ts": ts,
        "kind": "contact_req",
        "name": name,
        "ack": None,
    }
    if ph is not None:
        obj["ph"] = ph
    return json.dumps(obj, separators=(",", ":")).encode("utf-8")


def _book_version(device_id: str) -> int:
    # `bookVersion` is written directly on `devices/{d}` by
    # `contacts_store.bump_book_version` (see that module's docstring for
    # why it isn't yet on `devices_store.Device`) -- read it back the same
    # raw way rather than through `devices_store.get_device`, which would
    # silently drop it.
    raw = get_db().collection("devices").document(device_id).get().to_dict() or {}
    return int(raw.get("bookVersion", 0))


# ---------------------------------------------------------------------------
# ingest: kind:"contact_req" (docs/PROTOCOL.md §3.2, §4.2)
# ---------------------------------------------------------------------------


def test_contact_req_with_phone_creates_pending_request():
    _make_user("student", "student")
    _make_pager_device("pgr-c-1", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-1"), contact_req_payload("u_c1", "Grandma", ph="+15551234567"))

    req = contacts_store.get_by_device_and_req("pgr-c-1", "u_c1")
    assert req is not None
    assert req.status == "pending"
    assert req.name == "Grandma"
    assert req.phone == "+15551234567"
    assert req.alias is None
    assert req.ownerUid == "student"
    assert broker.published == []


def test_contact_req_with_alias_reference_is_stored():
    """An alias outside the family whose person has a `message` edge to the
    owner is a pending link request (docs/CONTACT_REQ_DESIGN.md decision 1)."""
    _make_user("student", "student", family_id="fam-a")
    _make_user("sibling", "sibling", family_id="fam-b")
    allow_store.set_edge("sibling", "student", message=True, locate=False)
    _make_pager_device("pgr-c-2", "student")
    ingest, _broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-2"), contact_req_payload("u_c2", "Sibling", ph="sibling"))

    req = contacts_store.get_by_device_and_req("pgr-c-2", "u_c2")
    assert req is not None
    assert req.status == "pending"
    assert req.alias == "sibling"
    assert req.phone is None


def test_contact_req_dedup_by_id_is_idempotent():
    _make_user("student", "student")
    _make_pager_device("pgr-c-3", "student")
    ingest, _broker = _ingest()

    payload = contact_req_payload("u_c3", "Uncle Bob", ph="+15559990000")
    ingest.handle_up(up_topic("pgr-c-3"), payload)
    ingest.handle_up(up_topic("pgr-c-3"), payload)

    assert contacts_store.count_pending("pgr-c-3") == 1


def test_contact_req_noop_when_phone_already_pending():
    _make_user("student", "student")
    _make_pager_device("pgr-c-4", "student")
    ingest, _broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-4"), contact_req_payload("u_c4a", "Grandma", ph="+15551110000"))
    ingest.handle_up(
        up_topic("pgr-c-4"), contact_req_payload("u_c4b", "Grandma Again", ph="+15551110000")
    )

    assert contacts_store.count_pending("pgr-c-4") == 1
    assert contacts_store.get_by_device_and_req("pgr-c-4", "u_c4b") is None


def test_contact_req_noop_when_phone_already_approved():
    _make_user("student", "student")
    _make_pager_device("pgr-c-5", "student")
    ingest, _broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-5"), contact_req_payload("u_c5a", "Grandma", ph="+15552220000"))
    contacts_store.approve(contacts_store.key("pgr-c-5", "u_c5a"), decided_by="admin1")

    ingest.handle_up(
        up_topic("pgr-c-5"), contact_req_payload("u_c5b", "Grandma Again", ph="+15552220000")
    )

    assert contacts_store.get_by_device_and_req("pgr-c-5", "u_c5b") is None
    assert contacts_store.count_pending("pgr-c-5") == 0


def test_contact_req_rate_limited_at_five_pending_sends_system_reply():
    _make_user("student", "student")
    _make_pager_device("pgr-c-6", "student")
    ingest, broker = _ingest()

    for i in range(5):
        ingest.handle_up(
            up_topic("pgr-c-6"),
            contact_req_payload(f"u_c6_{i}", f"Contact{i}", ph=f"+1555000000{i}"),
        )
    assert contacts_store.count_pending("pgr-c-6") == 5
    assert broker.published == []

    ingest.handle_up(
        up_topic("pgr-c-6"), contact_req_payload("u_c6_x", "One Too Many", ph="+15559999999")
    )

    assert contacts_store.count_pending("pgr-c-6") == 5
    assert contacts_store.get_by_device_and_req("pgr-c-6", "u_c6_x") is None
    assert len(broker.published) == 1
    sent = json.loads(broker.published[0].payload)
    assert sent["from"] == "system"
    assert sent["body"] == "too many pending requests"


def test_contact_req_from_unregistered_device_dropped():
    ingest, broker = _ingest()
    ingest.handle_up(up_topic("pgr-c-missing"), contact_req_payload("u_c7", "Nobody"))
    assert contacts_store.get_by_device_and_req("pgr-c-missing", "u_c7") is None
    assert broker.published == []


def test_contact_req_from_revoked_device_dropped():
    _make_user("student", "student")
    _make_pager_device("pgr-c-8", "student")
    devices_store.revoke_device("pgr-c-8")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-8"), contact_req_payload("u_c8", "Nope"))
    assert contacts_store.get_by_device_and_req("pgr-c-8", "u_c8") is None
    assert broker.published == []


def test_contact_req_name_too_long_is_dropped_as_malformed():
    _make_user("student", "student")
    _make_pager_device("pgr-c-9", "student")
    ingest, _broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-9"), contact_req_payload("u_c9", "N" * 17))
    assert contacts_store.get_by_device_and_req("pgr-c-9", "u_c9") is None


def test_contact_req_bad_ph_format_is_rejected_with_a_reply():
    """A bad `ph` is answered, not silently malformed-dropped (decision 1)."""
    _make_user("student", "student")
    _make_pager_device("pgr-c-10", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-10"), contact_req_payload("u_c10", "Bad Phone", ph="+abc"))
    req = contacts_store.get_by_device_and_req("pgr-c-10", "u_c10")
    assert req is not None and req.status == "rejected" and req.reason == "bad_number"
    assert [json.loads(m.payload)["body"] for m in broker.published if b'"system"' in m.payload] == [
        "Bad Phone: number must be 10 digits or start with +"
    ]


def test_hmac_signed_contact_req_is_verified_then_stored():
    _make_user("student", "student")
    key = _make_hmac_pager_device("pgr-c-11", "student")
    ingest, _broker = _ingest()

    topic = up_topic("pgr-c-11")
    payload = devauth.sign_json(
        key,
        topic,
        {
            "v": 1,
            "id": "u_c11",
            "ts": int(time.time()),
            "kind": "contact_req",
            "name": "Aunt",
            "ph": "+15553330000",
            "ack": None,
            "n": 1,
        },
    )
    ingest.handle_up(topic, payload)

    req = contacts_store.get_by_device_and_req("pgr-c-11", "u_c11")
    assert req is not None
    assert req.phone == "+15553330000"


# ---------------------------------------------------------------------------
# family alerts: POST /api/family/alerts/{id}/{approve|block} for the
# `contact_request` kind (docs/FAMILIES_TASKS.md 4.1, 5.1); admin:
# /api/admin/users/{uid}/backends
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


@pytest.fixture
def admin_headers() -> dict[str, str]:
    auth_user = fb_auth.create_user(email="contacts-admin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="contactsadmin", display_name="Admin", role="super"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"role": "super", "fam": ""})
    return auth_header(auth_user.uid)


def _make_family(name: str) -> families_store.Family:
    return families_store.create_family(name=name, created_by="root-contacts")


def _make_family_admin(uid: str, alias: str, family_id: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(
        uid=uid, alias=alias, display_name=alias, role="admin", family_id=family_id
    )
    fb_auth.set_custom_user_claims(uid, {"role": "admin", "fam": family_id})
    return auth_header(uid)


def _open_contact_request_alert(family_id: str):
    """The single open `contact_request` alert `app/alerts.py`'s
    `contact_request` (called from `contacts_store.create_request`) raised
    for `family_id` -- every test below sets up exactly one pending
    request, so there is exactly one."""
    alerts = alerts_store.list_alerts(family_id, "all")
    assert len(alerts) == 1
    assert alerts[0].kind == "contact_request"
    return alerts[0]


def test_approve_link_to_existing_verified_phone(client: TestClient):
    family = _make_family("Link1")
    headers = _make_family_admin("cadmin1", "cadmin1", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-2", "student")
    users_store.create_user(
        uid="grandma1", alias="grandma1", display_name="Grandma", family_id="gma-fam"
    )
    backend = backends_store.create_backend(
        "grandma1", kind="sms", config={"phone": "+15555550000"}
    )
    backends_store.update_backend("grandma1", backend.id, verified=True)
    backends_store.set_phone_index("+15555550000", "grandma1", backend.id)
    # Grandma already has an edge to the student, with `locate`: approval
    # must not rewrite it (decision 2).
    allow_store.set_edge("grandma1", "student", message=True, locate=True)

    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-2"), contact_req_payload("u_a2", "Grandma", ph="+15555550000"))

    alert = _open_contact_request_alert(family.id)
    # docs/FAMILIES_TASKS.md 3.2 addition (b): contact approval always
    # writes message-only edges now, so a `locate` in the request body (if
    # a stale client still sends one) is simply ignored, not honoured.
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve",
        json={"mode": "link", "locate": True},
        headers=headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"

    assert allow_store.is_message_allowed("student", "grandma1")
    inbound = allow_store.get_edge("grandma1", "student")
    assert inbound is not None and inbound.message and inbound.locate is True
    edge = allow_store.get_edge("student", "grandma1")
    assert edge is not None and edge.message and edge.locate is False

    request = contacts_store.get_by_device_and_req("pgr-a-2", "u_a2")
    assert request is not None and request.status == "approved"
    assert _book_version("pgr-a-2") == 1


def test_approve_sms_creates_one_family_contact_and_no_auth_user(client: TestClient):
    family = _make_family("Create1")
    headers = _make_family_admin("cadmin2", "cadmin2", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-3", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-3"), contact_req_payload("u_a3", "Auntie", ph="5556660000"))
    assert contacts_store.get_by_device_and_req("pgr-a-3", "u_a3").phone == "+15556660000"

    alert = _open_contact_request_alert(family.id)
    auth_before = len(list(fb_auth.list_users().iterate_all()))
    externals_before = [u for u in users_store.list_users() if u.kind == "external"]
    assert externals_before == []

    resp = client.post(f"/api/family/alerts/{alert.id}/approve", json={}, headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"

    externals = [u for u in users_store.list_users() if u.kind == "external"]
    assert len(externals) == 1
    ext = externals[0]
    assert ext.displayName == "Auntie"
    assert ext.phone == "+15556660000"
    assert ext.ownerFamilyId == family.id and ext.familyId is None
    assert (ext.uid, ext.alias) == externals_store.contact_ids(family.id, "+15556660000")
    assert len(list(fb_auth.list_users().iterate_all())) == auth_before
    # No person was created either.
    assert users_store.get_uid_for_alias("auntie") is None

    assert allow_store.is_message_allowed("student", ext.uid)
    assert not allow_store.is_message_allowed(ext.uid, "student")
    device = devices_store.get_device("pgr-a-3")
    assert device is not None
    assert [c.phone for c in device.smsContacts] == ["+15556660000"]
    request = contacts_store.get_by_device_and_req("pgr-a-3", "u_a3")
    assert request is not None and request.status == "approved"
    assert _book_version("pgr-a-3") >= 1


def test_approve_mode_create_is_refused(client: TestClient):
    family = _make_family("Create2")
    headers = _make_family_admin("cadmin3", "cadmin3", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-4", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-4"), contact_req_payload("u_a4", "Kid", ph="2065550100"))

    alert = _open_contact_request_alert(family.id)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"mode": "create"}, headers=headers
    )
    assert resp.status_code == 400
    assert resp.json()["detail"] == "new people are added under Family > People"
    request = contacts_store.get_by_device_and_req("pgr-a-4", "u_a4")
    assert request is not None and request.status == "pending"


def test_approve_link_after_the_inbound_edge_is_gone_is_409(client: TestClient):
    family = _make_family("Link2")
    headers = _make_family_admin("cadmin4", "cadmin4", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_user("gma", "gma", family_id="gma-fam")
    allow_store.set_edge("gma", "student", message=True, locate=False)
    _make_pager_device("pgr-a-5", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-5"), contact_req_payload("u_a5", "Gma", ph="gma"))
    alert = _open_contact_request_alert(family.id)
    assert alert.peerAlias == "gma" and alert.peerUid == "gma"

    allow_store.delete_edge("gma", "student")
    resp = client.post(f"/api/family/alerts/{alert.id}/approve", json={}, headers=headers)
    assert resp.status_code == 409, resp.text
    assert resp.json()["detail"] == "@gma no longer has an edge to @student"
    request = contacts_store.get_by_device_and_req("pgr-a-5", "u_a5")
    assert request is not None and request.status == "pending"
    assert allow_store.get_edge("student", "gma") is None


def test_block_contact_request_alert_adds_blocked_number_and_marks_handled(client: TestClient):
    """docs/FAMILIES_DESIGN.md §10 item 8: Block replaces the old
    Reject-with-reason for `contact_request` alerts. `block_alert`
    (`app/routers/family.py`) adds the alert's `peerPhone` to
    `families.blockedNumbers`, marks the alert `handled`, *and* -- same as
    the deleted `POST /api/admin/contacts/{key}/reject` route -- rejects the
    underlying `contactRequests` doc and bumps/republishes the device's
    book, so the pager's 5-pending cap and its book both reflect the
    decision."""
    family = _make_family("Block1")
    headers = _make_family_admin("cadmin5", "cadmin5", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-6", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-6"), contact_req_payload("u_a6", "Stranger", ph="+15557770000"))

    alert = _open_contact_request_alert(family.id)
    resp = client.post(f"/api/family/alerts/{alert.id}/block", headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"

    updated_family = families_store.get_family(family.id)
    assert updated_family is not None
    assert "+15557770000" in updated_family.blockedNumbers

    request = contacts_store.get_by_device_and_req("pgr-a-6", "u_a6")
    assert request is not None
    assert request.status == "rejected"
    assert request.reason == "blocked"
    assert request.decidedBy == "cadmin5"
    assert _book_version("pgr-a-6") == 1


def test_dismiss_contact_request_alert_rejects_request_and_publishes_book(client: TestClient):
    """`dismiss_alert` (`app/routers/family.py`) has no `peerPhone`/
    `blockedNumbers` step, but for a `contact_request` alert it must still
    reject the underlying `contactRequests` doc and bump/republish the
    device's book, the same as `block_alert` above (docs/FAMILIES_TASKS.md
    4.1)."""
    family = _make_family("Dismiss1")
    headers = _make_family_admin("cadmin8", "cadmin8", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-8", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-8"), contact_req_payload("u_a8", "Rando", ph="+15559990001"))

    alert = _open_contact_request_alert(family.id)
    resp = client.post(f"/api/family/alerts/{alert.id}/dismiss", headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "dismissed"

    request = contacts_store.get_by_device_and_req("pgr-a-8", "u_a8")
    assert request is not None
    assert request.status == "rejected"
    assert request.reason == "dismissed"
    assert request.decidedBy == "cadmin8"
    assert _book_version("pgr-a-8") == 1


def test_approve_already_decided_is_conflict(client: TestClient):
    family = _make_family("Conflict1")
    headers = _make_family_admin("cadmin7", "cadmin7", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-7", "student")
    users_store.create_user(
        uid="grandma7", alias="grandma7", display_name="Grandma7", family_id="gma7-fam"
    )
    allow_store.set_edge("grandma7", "student", message=True, locate=False)
    backend = backends_store.create_backend(
        "grandma7", kind="sms", config={"phone": "+15558880000"}
    )
    backends_store.update_backend("grandma7", backend.id, verified=True)
    backends_store.set_phone_index("+15558880000", "grandma7", backend.id)
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-7"), contact_req_payload("u_a7", "Once", ph="+15558880000"))

    alert = _open_contact_request_alert(family.id)
    resp1 = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"mode": "link"}, headers=headers
    )
    assert resp1.status_code == 200

    resp2 = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"mode": "link"}, headers=headers
    )
    assert resp2.status_code == 409


def test_create_user_backend_sets_verified_and_phone_index(
    client: TestClient, admin_headers: dict[str, str]
):
    user = users_store.create_user(uid="backenduser1", alias="backenduser1", display_name="BU")
    resp = client.post(
        f"/api/admin/users/{user.uid}/backends",
        json={"kind": "sms", "phone": "+15559990000"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["kind"] == "sms"
    assert data["verifiedAt"] is not None

    found = backends_store.get_by_phone("+15559990000")
    assert found is not None
    assert found[0] == user.uid

    raw = (
        get_db()
        .collection("users")
        .document(user.uid)
        .collection("backends")
        .document(data["id"])
        .get()
        .to_dict()
    )
    assert raw is not None and raw["adminVerified"] is True


# ---------------------------------------------------------------------------
# classify_ph + the decision-1 outcome table (docs/CONTACT_REQ_DESIGN.md)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("ph", "expected"),
    [
        ("2065550100", ("phone", "+12065550100")),
        ("12065550100", ("phone", "+12065550100")),
        ("+442071838750", ("phone", "+442071838750")),
        ("+12065550100", ("phone", "+12065550100")),
        ("+1206", ("bad", "+1206")),
        ("5550100", ("bad", "5550100")),
        ("123456789012", ("bad", "123456789012")),
        ("grandma", ("alias", "grandma")),
        ("Grandma", ("bad", "Grandma")),
        ("+abc", ("bad", "+abc")),
    ],
)
def test_classify_ph(ph: str, expected: tuple[str, str]):
    assert classify_ph(ph) == expected


def _system_bodies(broker: FakeBrokerClient) -> list[str]:
    out = []
    for m in broker.published:
        d = json.loads(m.payload)
        if d.get("from") == "system":
            out.append(d["body"])
    return out


def _book_pushes(broker: FakeBrokerClient) -> int:
    return sum(1 for m in broker.published if json.loads(m.payload).get("kind") == "book")


def test_bare_eleven_digit_ph_is_a_pending_phone_request_with_one_alert():
    family = _make_family("Cls1")
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-k-1", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-k-1"), contact_req_payload("u_k1", "Gma", ph="12065550100"))

    req = contacts_store.get_by_device_and_req("pgr-k-1", "u_k1")
    assert req is not None and req.status == "pending"
    assert req.phone == "+12065550100" and req.alias is None
    assert len(alerts_store.list_alerts(family.id, "all")) == 1
    assert _system_bodies(broker) == []
    # No person (or anything else) was created from the digits.
    assert [u.uid for u in users_store.list_users()] == ["student"]


def test_unknown_alias_is_rejected_no_contact_with_one_reply():
    family = _make_family("Cls2")
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-k-2", "student")
    ingest, broker = _ingest()
    bv = _book_version("pgr-k-2")

    ingest.handle_up(up_topic("pgr-k-2"), contact_req_payload("u_k2", "X", ph="nobody"))

    req = contacts_store.get_by_device_and_req("pgr-k-2", "u_k2")
    assert req is not None
    assert (req.status, req.reason, req.decidedBy) == ("rejected", "no_contact", "relay")
    assert _system_bodies(broker) == ["X: no contact @nobody"]
    assert _book_version("pgr-k-2") == bv + 1
    assert _book_pushes(broker) == 1
    assert alerts_store.list_alerts(family.id, "all") == []
    assert users_store.get_uid_for_alias("nobody") is None
    assert [u.uid for u in users_store.list_users()] == ["student"]


def test_alias_without_an_inbound_edge_looks_the_same_as_unknown():
    """One body for every alias failure: no probing which aliases exist."""
    _make_user("student", "student", family_id="fam-a")
    _make_user("stranger", "stranger", family_id="fam-b")
    _make_pager_device("pgr-k-3", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-k-3"), contact_req_payload("u_k3", "X", ph="stranger"))
    ingest.handle_up(up_topic("pgr-k-3"), contact_req_payload("u_k3b", "X", ph="nobody"))
    bodies = _system_bodies(broker)
    assert bodies == ["X: no contact @stranger", "X: no contact @nobody"]


def test_same_family_alias_is_already_in_your_book_with_no_row():
    family = _make_family("Cls4")
    _make_user("student", "student", family_id=family.id)
    _make_user("sis", "sis", family_id=family.id)
    _make_pager_device("pgr-k-4", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-k-4"), contact_req_payload("u_k4", "X", ph="sis"))

    assert contacts_store.get_by_device_and_req("pgr-k-4", "u_k4") is None
    assert _system_bodies(broker) == ["X: already in your book"]
    assert alerts_store.list_alerts(family.id, "all") == []


def test_bad_number_and_blocked_number_are_rejected_with_their_bodies():
    family = _make_family("Cls5")
    families_store.add_blocked_number(family.id, "+12065550999")
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-k-5", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-k-5"), contact_req_payload("u_k5a", "Bad", ph="5550100"))
    ingest.handle_up(up_topic("pgr-k-5"), contact_req_payload("u_k5b", "Blk", ph="2065550999"))

    a = contacts_store.get_by_device_and_req("pgr-k-5", "u_k5a")
    b = contacts_store.get_by_device_and_req("pgr-k-5", "u_k5b")
    assert a is not None and (a.status, a.reason) == ("rejected", "bad_number")
    assert b is not None and (b.status, b.reason) == ("rejected", "blocked")
    assert _system_bodies(broker) == [
        "Bad: number must be 10 digits or start with +",
        "Blk: number not allowed",
    ]
    assert alerts_store.list_alerts(family.id, "all") == []


def test_phone_already_a_family_contact_with_an_edge_is_in_your_book():
    family = _make_family("Cls6")
    _make_user("student", "student", family_id=family.id)
    ext = externals_store.get_or_create(family.id, "+12065550123", "Gma")
    allow_store.set_edge("student", ext.uid, message=True, locate=False)
    _make_pager_device("pgr-k-6", "student")
    ingest, broker = _ingest()

    ingest.handle_up(up_topic("pgr-k-6"), contact_req_payload("u_k6", "Gma", ph="2065550123"))

    assert contacts_store.get_by_device_and_req("pgr-k-6", "u_k6") is None
    assert _system_bodies(broker) == ["Gma: already in your book"]


def test_redelivered_contact_req_makes_no_second_row_alert_or_reply():
    family = _make_family("Cls7")
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-k-7", "student")
    ingest, broker = _ingest()

    ok = contact_req_payload("u_k7a", "Ok", ph="2065550101")
    rej = contact_req_payload("u_k7b", "Rej", ph="nobody")
    for payload in (ok, rej):
        ingest.handle_up(up_topic("pgr-k-7"), payload)
    published = len(broker.published)
    bv = _book_version("pgr-k-7")
    for payload in (ok, rej):
        ingest.handle_up(up_topic("pgr-k-7"), payload)

    assert len(broker.published) == published
    assert _book_version("pgr-k-7") == bv
    assert len(alerts_store.list_alerts(family.id, "all")) == 1
    assert len(contacts_store.list_requests(device_id="pgr-k-7")) == 2


def test_create_request_losing_the_create_race_raises_no_alert(monkeypatch):
    """`create()` raising AlreadyExists (a concurrent delivery won) returns
    the stored row and raises no alert of its own."""
    family = _make_family("Cls8")
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-k-8", "student")
    first = contacts_store.create_request(
        device_id="pgr-k-8", owner_uid="student", req_id="u_k8", name="A", phone="+12065550102"
    )
    assert first is not None
    # Hide the row from the fast-path read so the write path is exercised.
    real_get = contacts_store.get_request
    calls = {"n": 0}

    def fake_get(doc_key: str):
        calls["n"] += 1
        return None if calls["n"] == 1 else real_get(doc_key)

    monkeypatch.setattr(contacts_store, "get_request", fake_get)
    monkeypatch.setattr(contacts_store, "has_matching_pending_or_approved", lambda *a, **k: False)
    again = contacts_store.create_request(
        device_id="pgr-k-8", owner_uid="student", req_id="u_k8", name="A", phone="+12065550102"
    )
    assert again is not None and again.key == first.key
    assert len(alerts_store.list_alerts(family.id, "all")) == 1

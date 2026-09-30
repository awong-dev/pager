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
from app.ingest import Ingest
from app.main import create_app
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
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
    _make_user("student", "student")
    _make_user("sibling", "sibling")
    _make_pager_device("pgr-c-2", "student")
    ingest, _broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-2"), contact_req_payload("u_c2", "Sibling", ph="sibling"))

    req = contacts_store.get_by_device_and_req("pgr-c-2", "u_c2")
    assert req is not None
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


def test_contact_req_bad_ph_format_is_dropped_as_malformed():
    _make_user("student", "student")
    _make_pager_device("pgr-c-10", "student")
    ingest, _broker = _ingest()

    ingest.handle_up(up_topic("pgr-c-10"), contact_req_payload("u_c10", "Bad Phone", ph="+abc"))
    assert contacts_store.get_by_device_and_req("pgr-c-10", "u_c10") is None


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
    users_store.create_user(uid="grandma1", alias="grandma1", display_name="Grandma")
    backend = backends_store.create_backend(
        "grandma1", kind="sms", config={"phone": "+15555550000"}
    )
    backends_store.update_backend("grandma1", backend.id, verified=True)
    backends_store.set_phone_index("+15555550000", "grandma1", backend.id)

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
    assert allow_store.is_message_allowed("grandma1", "student")
    edge = allow_store.get_edge("student", "grandma1")
    assert edge is not None and edge.locate is False

    request = contacts_store.get_by_device_and_req("pgr-a-2", "u_a2")
    assert request is not None and request.status == "approved"
    assert _book_version("pgr-a-2") == 1


def test_approve_create_creates_user_backend_and_edges(client: TestClient):
    family = _make_family("Create1")
    headers = _make_family_admin("cadmin2", "cadmin2", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-3", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-3"), contact_req_payload("u_a3", "Auntie", ph="+15556660000"))

    alert = _open_contact_request_alert(family.id)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve",
        json={"mode": "create", "alias": "auntie"},
        headers=headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"

    new_uid = users_store.get_uid_for_alias("auntie")
    assert new_uid is not None
    user = users_store.get_user(new_uid)
    assert user is not None and user.displayName == "Auntie"

    found = backends_store.get_by_phone("+15556660000")
    assert found is not None
    assert found[0] == new_uid

    assert allow_store.is_message_allowed("student", new_uid)
    assert allow_store.is_message_allowed(new_uid, "student")
    assert _book_version("pgr-a-3") == 1


def test_approve_create_requires_alias_when_none_can_be_derived(client: TestClient):
    family = _make_family("Create2")
    headers = _make_family_admin("cadmin3", "cadmin3", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-4", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-4"), contact_req_payload("u_a4", "小明"))

    alert = _open_contact_request_alert(family.id)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"mode": "create"}, headers=headers
    )
    assert resp.status_code == 400


def test_approve_link_without_a_match_is_400(client: TestClient):
    family = _make_family("Link2")
    headers = _make_family_admin("cadmin4", "cadmin4", family.id)
    _make_user("student", "student", family_id=family.id)
    _make_pager_device("pgr-a-5", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-5"), contact_req_payload("u_a5", "Ghost", ph="+15550001234"))

    alert = _open_contact_request_alert(family.id)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"mode": "link"}, headers=headers
    )
    assert resp.status_code == 400


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
    users_store.create_user(uid="grandma7", alias="grandma7", display_name="Grandma7")
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

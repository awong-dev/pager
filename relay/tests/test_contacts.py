"""`kind:"contact_req"` ingest handling (docs/PROTOCOL.md §3.2, §4.2) and the
admin approve/reject flow (docs/DEVICE_PLAN.md §4.3) -- docs/DEVICE_TASKS.md
S4.1.

Ingest-level tests exercise `app.ingest.Ingest.handle_up` directly, the same
style `tests/test_ingest.py` uses; admin-level tests go through the real
FastAPI app (`tests/test_admin.py`'s style) so the rate limiting / auth
dependencies are exercised too.
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
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import users as users_store
from tests.conftest import up_topic
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

_HMAC_KEY = b"k" * 32


# ---------------------------------------------------------------------------
# shared helpers
# ---------------------------------------------------------------------------


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


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
# admin: /api/admin/contacts, /api/admin/users/{uid}/backends
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
        uid=auth_user.uid, alias="contactsadmin", display_name="Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    return auth_header(auth_user.uid)


def test_list_contacts_filters_by_status(client: TestClient, admin_headers: dict[str, str]):
    _make_user("student", "student")
    _make_pager_device("pgr-a-1", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-1"), contact_req_payload("u_a1", "Grandma", ph="+15554440000"))

    resp = client.get("/api/admin/contacts?status=pending", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert len(data) == 1
    assert data[0]["name"] == "Grandma"

    resp2 = client.get("/api/admin/contacts?status=approved", headers=admin_headers)
    assert resp2.json() == []


def test_contacts_endpoints_require_admin(client: TestClient):
    fb_auth.create_user(uid="nonadmin1", email="nonadmin1@example.com")
    users_store.create_user(uid="nonadmin1", alias="nonadmin1", display_name="NA")
    resp = client.get("/api/admin/contacts", headers=auth_header("nonadmin1"))
    assert resp.status_code == 403


def test_approve_link_to_existing_verified_phone(client: TestClient, admin_headers: dict[str, str]):
    _make_user("student", "student")
    _make_pager_device("pgr-a-2", "student")
    users_store.create_user(uid="grandma1", alias="grandma1", display_name="Grandma")
    backend = backends_store.create_backend(
        "grandma1", kind="sms", config={"phone": "+15555550000"}
    )
    backends_store.update_backend("grandma1", backend.id, verified=True)
    backends_store.set_phone_index("+15555550000", "grandma1", backend.id)

    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-2"), contact_req_payload("u_a2", "Grandma", ph="+15555550000"))

    key = contacts_store.key("pgr-a-2", "u_a2")
    resp = client.post(
        f"/api/admin/contacts/{key}/approve",
        json={"mode": "link", "locate": True},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "approved"

    assert allow_store.is_message_allowed("student", "grandma1")
    assert allow_store.is_message_allowed("grandma1", "student")
    edge = allow_store.get_edge("student", "grandma1")
    assert edge is not None and edge.locate is True

    assert _book_version("pgr-a-2") == 1


def test_approve_create_creates_user_backend_and_edges(
    client: TestClient, admin_headers: dict[str, str]
):
    _make_user("student", "student")
    _make_pager_device("pgr-a-3", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-3"), contact_req_payload("u_a3", "Auntie", ph="+15556660000"))

    key = contacts_store.key("pgr-a-3", "u_a3")
    resp = client.post(
        f"/api/admin/contacts/{key}/approve",
        json={"mode": "create", "alias": "auntie"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "approved"

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


def test_approve_create_requires_alias_when_none_can_be_derived(
    client: TestClient, admin_headers: dict[str, str]
):
    _make_user("student", "student")
    _make_pager_device("pgr-a-4", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-4"), contact_req_payload("u_a4", "小明"))

    key = contacts_store.key("pgr-a-4", "u_a4")
    resp = client.post(
        f"/api/admin/contacts/{key}/approve", json={"mode": "create"}, headers=admin_headers
    )
    assert resp.status_code == 400


def test_approve_link_without_a_match_is_400(client: TestClient, admin_headers: dict[str, str]):
    _make_user("student", "student")
    _make_pager_device("pgr-a-5", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-5"), contact_req_payload("u_a5", "Ghost", ph="+15550001234"))

    key = contacts_store.key("pgr-a-5", "u_a5")
    resp = client.post(
        f"/api/admin/contacts/{key}/approve", json={"mode": "link"}, headers=admin_headers
    )
    assert resp.status_code == 400


def test_reject_stores_reason_and_bumps_book(client: TestClient, admin_headers: dict[str, str]):
    _make_user("student", "student")
    _make_pager_device("pgr-a-6", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-6"), contact_req_payload("u_a6", "Stranger", ph="+15557770000"))

    key = contacts_store.key("pgr-a-6", "u_a6")
    resp = client.post(
        f"/api/admin/contacts/{key}/reject",
        json={"reason": "not_allowed"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["status"] == "rejected"
    assert data["reason"] == "not_allowed"
    assert _book_version("pgr-a-6") == 1


def test_approve_already_decided_is_conflict(client: TestClient, admin_headers: dict[str, str]):
    _make_user("student", "student")
    _make_pager_device("pgr-a-7", "student")
    ingest, _broker = _ingest()
    ingest.handle_up(up_topic("pgr-a-7"), contact_req_payload("u_a7", "Once", ph="+15558880000"))

    key = contacts_store.key("pgr-a-7", "u_a7")
    resp1 = client.post(
        f"/api/admin/contacts/{key}/reject", json={"reason": "x"}, headers=admin_headers
    )
    assert resp1.status_code == 200

    resp2 = client.post(
        f"/api/admin/contacts/{key}/approve", json={"mode": "link"}, headers=admin_headers
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

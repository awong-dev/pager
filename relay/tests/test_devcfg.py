"""`app.devcfg`'s `book`/`cfg` down-envelope builders and push/ack/republish
plumbing -- docs/DEVICE_PLAN.md §4.3 (book), §5.8 (cfg), docs/PROTOCOL.md
§3.1/§3.2, docs/DEVICE_TASKS.md S4.2.

Style split, same as tests/test_contacts.py: `app.devcfg` functions are
exercised directly (fast, no HTTP), the admin `/cfg` route and the
approve/reject wiring go through the real FastAPI app so auth/rate-limit
dependencies are covered too.
"""

from __future__ import annotations

import base64
import hashlib
import json
import time
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import ca_resolve, devcfg
from app.config import Settings
from app.db.firestore import get_db
from app.ingest import Ingest
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import users as users_store
from tests.conftest import (
    ack_payload,
    offline_status_payload,
    online_status_payload,
    status_topic,
    up_topic,
)
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

# ---------------------------------------------------------------------------
# shared helpers
# ---------------------------------------------------------------------------


def _make_user(uid: str, alias: str, display_name: str | None = None) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=display_name or alias)


def _make_pager_device(
    device_id: str, owner_uid: str, *, default_to_uid: str | None = None
) -> None:
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        default_to_uid=default_to_uid,
        auth_mode="password",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def _approve(owner_uid: str, contact_uid: str) -> None:
    allow_store.set_edge(owner_uid, contact_uid, message=True, locate=False)
    allow_store.set_edge(contact_uid, owner_uid, message=True, locate=False)


def _raw_device(device_id: str) -> dict:
    return get_db().collection("devices").document(device_id).get().to_dict() or {}


def _bump_book_version(device_id: str) -> int:
    return contacts_store.bump_book_version(device_id)


# ---------------------------------------------------------------------------
# build_book
# ---------------------------------------------------------------------------


def test_build_book_basic_shape():
    _make_user("student1", "student1")
    _make_user("mom1", "mom1", "Mom")
    _make_pager_device("pgr-b-1", "student1", default_to_uid="mom1")
    _approve("student1", "mom1")
    _bump_book_version("pgr-b-1")

    obj = devcfg.build_book("pgr-b-1")

    assert obj["v"] == 1
    assert obj["kind"] == "book"
    assert obj["bv"] == 1
    assert obj["d"] == "mom1"
    assert obj["c"] == [{"a": "mom1", "n": "Mom", "t": "web"}]
    assert obj["p"] == []
    assert obj["ack"] is None
    assert obj["id"].startswith("m_")


def test_build_book_omits_d_when_no_default_recipient():
    _make_user("student2", "student2")
    _make_pager_device("pgr-b-2", "student2")

    obj = devcfg.build_book("pgr-b-2")

    assert "d" not in obj


def test_build_book_unregistered_device_raises():
    with pytest.raises(ValueError):
        devcfg.build_book("no-such-device")


def test_build_book_contact_type_hint_sms_over_web():
    _make_user("student3", "student3")
    _make_user("grandma3", "grandma3", "Grandma")
    backend = backends_store.create_backend(
        "grandma3", kind="sms", config={"phone": "+15550001111"}
    )
    backends_store.update_backend("grandma3", backend.id, verified=True)
    _make_pager_device("pgr-b-3", "student3")
    _approve("student3", "grandma3")

    obj = devcfg.build_book("pgr-b-3")

    assert obj["c"] == [{"a": "grandma3", "n": "Grandma", "t": "sms"}]


def test_build_book_caps_approved_contacts_at_ten():
    _make_user("student4", "student4")
    _make_pager_device("pgr-b-4", "student4")
    for i in range(12):
        uid = f"contact4_{i}"
        _make_user(uid, f"c4{i:02d}")
        _approve("student4", uid)

    obj = devcfg.build_book("pgr-b-4")

    assert len(obj["c"]) == 10


def test_build_book_caps_and_orders_pending_requests_newest_first():
    _make_user("student5", "student5")
    _make_pager_device("pgr-b-5", "student5")
    # `contactRequests` rate-limits to 5 *pending* per device (§3.2); reject
    # one and add a fresh one so there are 5 non-approved requests total
    # (4 pending + 1 rejected) spanning more than `MAX_LISTED_REQUESTS`.
    for i in range(5):
        contacts_store.create_request(
            device_id="pgr-b-5",
            owner_uid="student5",
            req_id=f"u_b5_{i}",
            name=f"Req{i}",
            phone=f"+1555000111{i}",
        )
    contacts_store.reject(
        contacts_store.key("pgr-b-5", "u_b5_0"), reason="x", decided_by="admin"
    )
    contacts_store.create_request(
        device_id="pgr-b-5",
        owner_uid="student5",
        req_id="u_b5_5",
        name="Req5",
        phone="+15550001115",
    )

    obj = devcfg.build_book("pgr-b-5")

    assert len(obj["p"]) == 4
    # Newest-first: the last four created (Req2..Req5) win over the oldest
    # (Req0, now rejected, and the only one not selected).
    names = {p["n"] for p in obj["p"]}
    assert names == {"Req2", "Req3", "Req4", "Req5"}


def test_build_book_lists_rejected_requests_as_no():
    _make_user("student6", "student6")
    _make_pager_device("pgr-b-6", "student6")
    contacts_store.create_request(
        device_id="pgr-b-6",
        owner_uid="student6",
        req_id="u_b6",
        name="Stranger",
        phone="+15559990000",
    )
    contacts_store.reject(contacts_store.key("pgr-b-6", "u_b6"), reason="x", decided_by="admin")

    obj = devcfg.build_book("pgr-b-6")

    assert obj["p"] == [{"n": "Stranger", "s": "no"}]


def test_build_book_excludes_approved_requests_from_pending_list():
    _make_user("student7", "student7")
    _make_pager_device("pgr-b-7", "student7")
    contacts_store.create_request(
        device_id="pgr-b-7",
        owner_uid="student7",
        req_id="u_b7",
        name="Auntie",
        phone="+15558880000",
    )
    contacts_store.approve(contacts_store.key("pgr-b-7", "u_b7"), decided_by="admin")

    obj = devcfg.build_book("pgr-b-7")

    assert obj["p"] == []


def test_build_book_lists_owners_group_as_grp_contact():
    """docs/GROUP_CHAT_DESIGN.md §4 (amended 23 Sep): a group the device's
    owner belongs to is listed in the book as `t: "grp"`, distinct from
    `web`/`sms`/`chat` -- the pick screen labels a row from
    `book_contact_t.type` verbatim."""
    _make_user("student8g", "student8g")
    _make_pager_device("pgr-b-8g", "student8g")
    _make_user("groupmate8g", "groupmate8g")
    conversations_store.create_group(
        name="Family", alias="fam-book", member_uids=["student8g", "groupmate8g"], created_by="student8g"
    )

    obj = devcfg.build_book("pgr-b-8g")

    assert {"a": "fam-book", "n": "Family", "t": "grp"} in obj["c"]


def test_build_book_group_contact_not_listed_for_non_member():
    _make_user("student9g", "student9g")
    _make_pager_device("pgr-b-9g", "student9g")
    _make_user("m1-9g", "m1-9g")
    _make_user("m2-9g", "m2-9g")
    conversations_store.create_group(
        name="Others", alias="fam-not-mine", member_uids=["m1-9g", "m2-9g"], created_by="m1-9g"
    )

    obj = devcfg.build_book("pgr-b-9g")

    assert obj["c"] == []


def test_build_book_caps_approved_contacts_at_ten_including_groups():
    _make_user("student10g", "student10g")
    _make_pager_device("pgr-b-10g", "student10g")
    for i in range(8):
        uid = f"contact10g_{i}"
        _make_user(uid, f"c10g{i:02d}")
        _approve("student10g", uid)
    for i in range(4):
        mate = f"groupmate10g_{i}"
        _make_user(mate, mate)
        conversations_store.create_group(
            name=f"Group{i}",
            alias=f"grp10g{i}",
            member_uids=["student10g", mate],
            created_by="student10g",
        )

    obj = devcfg.build_book("pgr-b-10g")

    assert len(obj["c"]) == 10
    assert any(c["t"] == "grp" for c in obj["c"])


# ---------------------------------------------------------------------------
# push_book / republish / ack
# ---------------------------------------------------------------------------


def test_push_book_publishes_and_stores_pending():
    _make_user("student8", "student8")
    _make_pager_device("pgr-b-8", "student8")
    broker = FakeBrokerClient()

    ok = devcfg.push_book("pgr-b-8", broker)

    assert ok is True
    assert len(broker.published) == 1
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "book"

    raw = _raw_device("pgr-b-8")
    assert raw["pendingBook"]["id"] == sent["id"]
    assert raw["pendingBook"]["acked"] is False


def test_push_book_unregistered_device_returns_false_without_publishing():
    broker = FakeBrokerClient()
    assert devcfg.push_book("no-such-device", broker) is False
    assert broker.published == []


def test_push_book_supersedes_older_unacked_book():
    _make_user("student9", "student9")
    _make_pager_device("pgr-b-9", "student9")
    broker = FakeBrokerClient()

    devcfg.push_book("pgr-b-9", broker)
    first_id = _raw_device("pgr-b-9")["pendingBook"]["id"]

    devcfg.push_book("pgr-b-9", broker)
    second_id = _raw_device("pgr-b-9")["pendingBook"]["id"]

    assert first_id != second_id
    # The superseded id is forgotten -- an ack for it is now indistinguishable
    # from an ack for a truly unknown id.
    assert devcfg.ack("pgr-b-9", first_id) is False
    assert devcfg.ack("pgr-b-9", second_id) is True


def test_ack_shown_marks_book_acked_idempotently():
    _make_user("student10", "student10")
    _make_pager_device("pgr-b-10", "student10")
    broker = FakeBrokerClient()
    devcfg.push_book("pgr-b-10", broker)
    msg_id = _raw_device("pgr-b-10")["pendingBook"]["id"]

    assert devcfg.ack("pgr-b-10", msg_id) is True
    assert _raw_device("pgr-b-10")["pendingBook"]["acked"] is True
    # Idempotent re-ack.
    assert devcfg.ack("pgr-b-10", msg_id) is True


def test_ack_unknown_id_returns_false():
    _make_user("student11", "student11")
    _make_pager_device("pgr-b-11", "student11")
    assert devcfg.ack("pgr-b-11", "m_doesnotexist") is False


def test_republish_pending_skips_acked_book():
    _make_user("student12", "student12")
    _make_pager_device("pgr-b-12", "student12")
    broker = FakeBrokerClient()
    devcfg.push_book("pgr-b-12", broker)
    msg_id = _raw_device("pgr-b-12")["pendingBook"]["id"]
    devcfg.ack("pgr-b-12", msg_id)
    broker.clear()

    devcfg.republish_pending("pgr-b-12", broker)

    assert broker.published == []


def test_republish_pending_resends_unacked_book_and_cfg_with_same_id():
    _make_user("student13", "student13")
    _make_pager_device("pgr-b-13", "student13")
    broker = FakeBrokerClient()
    devcfg.push_book("pgr-b-13", broker)
    devcfg.push_cfg("pgr-b-13", {"auto": 10}, broker)
    book_id = _raw_device("pgr-b-13")["pendingBook"]["id"]
    cfg_id = _raw_device("pgr-b-13")["pendingCfg"]["id"]
    broker.clear()

    devcfg.republish_pending("pgr-b-13", broker)

    assert len(broker.published) == 2
    sent_ids = {json.loads(p.payload)["id"] for p in broker.published}
    assert sent_ids == {book_id, cfg_id}


# ---------------------------------------------------------------------------
# push_cfg
# ---------------------------------------------------------------------------


def test_push_cfg_publishes_lock_and_stores_pending():
    _make_user("student14", "student14")
    _make_pager_device("pgr-b-14", "student14")
    broker = FakeBrokerClient()

    ok = devcfg.push_cfg("pgr-b-14", {"clear": True}, broker)

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    assert sent["cfg"] == {"lock": {"clear": True}}
    assert sent["ack"] is None

    raw = _raw_device("pgr-b-14")
    assert raw["pendingCfg"]["id"] == sent["id"]
    assert raw["pendingCfg"]["acked"] is False


def test_push_cfg_unregistered_device_returns_false():
    broker = FakeBrokerClient()
    assert devcfg.push_cfg("no-such-device", {"auto": 5}, broker) is False
    assert broker.published == []


# ---------------------------------------------------------------------------
# push_ca / unpin_ca -- docs/V02_DESIGN.md §4.4.
# ---------------------------------------------------------------------------

_CA_PEM = "-----BEGIN CERTIFICATE-----\nMIIB...FAKE...TEST...CA==\n-----END CERTIFICATE-----\n"


def _settings_with_public_base_url() -> Settings:
    return Settings(
        broker_api_url="http://emqx.test/api/v5",
        broker_api_key="k",
        broker_api_secret="s",
        webhook_key="wk",
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
        public_base_url="https://relay.example.com",
    )


def test_push_ca_publishes_pointer_and_stores_pending():
    _make_user("student19", "student19")
    _make_pager_device("pgr-ca-1", "student19")
    broker = FakeBrokerClient()

    ok = devcfg.push_ca(
        "pgr-ca-1", pem=_CA_PEM, broker=broker, settings=_settings_with_public_base_url()
    )

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    sha_hex = hashlib.sha256(_CA_PEM.encode()).hexdigest()
    assert sent["cfg"]["ca"]["url"] == f"https://relay.example.com/ca/{sha_hex}.pem"
    # JSON wire: sha is base64url, per docs/V02_DESIGN.md §7.
    assert sent["cfg"]["ca"]["sha"] == base64.urlsafe_b64encode(
        bytes.fromhex(sha_hex)
    ).rstrip(b"=").decode()

    raw = _raw_device("pgr-ca-1")
    assert raw["pendingCfgCa"]["id"] == sent["id"]
    assert raw["pendingCfgCa"]["acked"] is False


def test_push_ca_unregistered_device_returns_false():
    broker = FakeBrokerClient()
    assert (
        devcfg.push_ca(
            "no-such-device", pem=_CA_PEM, broker=broker, settings=_settings_with_public_base_url()
        )
        is False
    )
    assert broker.published == []


def test_push_ca_without_public_base_url_raises():
    _make_user("student20", "student20")
    _make_pager_device("pgr-ca-2", "student20")
    broker = FakeBrokerClient()
    settings = Settings(
        broker_api_url="http://emqx.test/api/v5",
        broker_api_key="k",
        broker_api_secret="s",
        webhook_key="wk",
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
        public_base_url=None,
    )
    with pytest.raises(ca_resolve.PublicBaseUrlRequired):
        devcfg.push_ca("pgr-ca-2", pem=_CA_PEM, broker=broker, settings=settings)
    assert broker.published == []


def test_unpin_ca_publishes_empty_url_no_sha():
    _make_user("student21", "student21")
    _make_pager_device("pgr-ca-3", "student21")
    broker = FakeBrokerClient()

    ok = devcfg.unpin_ca("pgr-ca-3", broker)

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"]["ca"] == {"url": ""}
    assert "sha" not in sent["cfg"]["ca"]


def test_unpin_ca_unregistered_device_returns_false():
    broker = FakeBrokerClient()
    assert devcfg.unpin_ca("no-such-device", broker) is False
    assert broker.published == []


def test_pending_cfg_lock_and_pending_cfg_ca_do_not_clobber_each_other():
    """docs/V02_DESIGN.md §4.4: "a pending `lock` and a pending `ca` must
    not clobber each other" -- both are independently the "newest unacked"
    of their own kind, and republishing sends both."""
    _make_user("student22", "student22")
    _make_pager_device("pgr-ca-4", "student22")
    broker = FakeBrokerClient()

    devcfg.push_cfg("pgr-ca-4", {"auto": 5}, broker)
    devcfg.push_ca(
        "pgr-ca-4", pem=_CA_PEM, broker=broker, settings=_settings_with_public_base_url()
    )
    lock_id = _raw_device("pgr-ca-4")["pendingCfg"]["id"]
    ca_id = _raw_device("pgr-ca-4")["pendingCfgCa"]["id"]
    assert lock_id != ca_id
    broker.clear()

    devcfg.republish_pending("pgr-ca-4", broker)

    assert len(broker.published) == 2
    sent_ids = {json.loads(p.payload)["id"] for p in broker.published}
    assert sent_ids == {lock_id, ca_id}

    # Acking the lock cfg does not ack the pending CA cfg, and vice versa.
    assert devcfg.ack("pgr-ca-4", lock_id) is True
    assert _raw_device("pgr-ca-4")["pendingCfgCa"]["acked"] is False
    assert devcfg.ack("pgr-ca-4", ca_id) is True
    assert _raw_device("pgr-ca-4")["pendingCfg"]["acked"] is True
    assert _raw_device("pgr-ca-4")["pendingCfgCa"]["acked"] is True


# ---------------------------------------------------------------------------
# ingest.py wiring: bv-behind-bookVersion, ack routing, online-edge republish
# ---------------------------------------------------------------------------


def test_status_bv_behind_book_version_triggers_push_book():
    _make_user("student15", "student15")
    _make_pager_device("pgr-b-15", "student15")
    _bump_book_version("pgr-b-15")
    _bump_book_version("pgr-b-15")  # bookVersion == 2
    broker = FakeBrokerClient()
    ingest = Ingest(broker)

    ingest.handle_status(
        status_topic("pgr-b-15"), online_status_payload("s_00000001", bv=1)
    )

    books = [
        json.loads(p.payload) for p in broker.published if json.loads(p.payload).get("kind") == "book"
    ]
    assert len(books) == 1
    assert books[0]["bv"] == 2


def test_status_bv_caught_up_does_not_push_book():
    _make_user("student16", "student16")
    _make_pager_device("pgr-b-16", "student16")
    _bump_book_version("pgr-b-16")
    broker = FakeBrokerClient()
    ingest = Ingest(broker)

    ingest.handle_status(
        status_topic("pgr-b-16"), online_status_payload("s_00000002", bv=1)
    )

    assert broker.published == []


def test_up_ack_shown_for_book_id_is_handled_by_devcfg_not_dropped():
    _make_user("student17", "student17")
    _make_pager_device("pgr-b-17", "student17")
    broker = FakeBrokerClient()
    devcfg.push_book("pgr-b-17", broker)
    book_id = _raw_device("pgr-b-17")["pendingBook"]["id"]
    ingest = Ingest(broker)

    ingest.handle_up(up_topic("pgr-b-17"), ack_payload(book_id, "shown"))

    assert _raw_device("pgr-b-17")["pendingBook"]["acked"] is True


def test_online_edge_republishes_unacked_book():
    _make_user("student18", "student18")
    _make_pager_device("pgr-b-18", "student18")
    broker = FakeBrokerClient()
    devcfg.push_book("pgr-b-18", broker)
    book_id = _raw_device("pgr-b-18")["pendingBook"]["id"]
    ingest = Ingest(broker)

    # First online (offline -> online edge) triggers a republish.
    ingest.handle_status(status_topic("pgr-b-18"), offline_status_payload("s_x"))
    broker.clear()
    ingest.handle_status(status_topic("pgr-b-18"), online_status_payload("s_00000003"))

    books = [
        json.loads(p.payload) for p in broker.published if json.loads(p.payload).get("kind") == "book"
    ]
    assert len(books) == 1
    assert books[0]["id"] == book_id


# ---------------------------------------------------------------------------
# admin: POST /api/admin/devices/{id}/cfg, and approve/reject -> devcfg.push_book
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
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def client(broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        yield c


@pytest.fixture
def admin_headers() -> dict[str, str]:
    auth_user = fb_auth.create_user(email="devcfg-admin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="devcfgadmin", display_name="Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    return auth_header(auth_user.uid)


def test_admin_push_cfg_endpoint(
    client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _make_user("student19", "student19")
    _make_pager_device("pgr-b-19", "student19")

    resp = client.post(
        "/api/admin/devices/pgr-b-19/cfg",
        json={"lock": {"clear": True, "auto": 5}},
        headers=admin_headers,
    )

    assert resp.status_code == 200, resp.text
    assert resp.json() == {"ok": True}
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    assert sent["cfg"] == {"lock": {"clear": True, "auto": 5}}


def test_admin_push_cfg_unknown_device_404(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post(
        "/api/admin/devices/no-such-device/cfg",
        json={"lock": {"clear": True}},
        headers=admin_headers,
    )
    assert resp.status_code == 404


def test_admin_push_cfg_requires_admin(client: TestClient):
    fb_auth.create_user(uid="nonadmin2", email="nonadmin2@example.com")
    users_store.create_user(uid="nonadmin2", alias="nonadmin2", display_name="NA")
    resp = client.post(
        "/api/admin/devices/pgr-b-x/cfg",
        json={"lock": {"clear": True}},
        headers=auth_header("nonadmin2"),
    )
    assert resp.status_code == 403


def _ca_client(broker: FakeBrokerClient, **settings_overrides: object) -> TestClient:
    """Same shape as the `client` fixture, but with `Settings` overrides --
    `POST /devices/{id}/ca` reads `settings.broker_ca_pem`/`public_base_url`
    from `app.state.settings` (set once at app construction), not from
    `os.environ` at request time, so these tests build their own app rather
    than monkeypatching the environment after the fact."""
    app = create_app(settings=make_settings(**settings_overrides), broker_client=broker)
    return TestClient(app)


def test_admin_push_ca_endpoint(admin_headers: dict[str, str], broker: FakeBrokerClient):
    _make_user("student-ca-1", "student-ca-1")
    _make_pager_device("pgr-adminca-1", "student-ca-1")

    with _ca_client(
        broker, broker_ca_pem=_CA_PEM, public_base_url="https://relay.example.com"
    ) as client:
        resp = client.post(
            "/api/admin/devices/pgr-adminca-1/ca", json={"action": "push"}, headers=admin_headers
        )

    assert resp.status_code == 200, resp.text
    assert resp.json() == {"ok": True}
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    sha_hex = hashlib.sha256(_CA_PEM.encode()).hexdigest()
    assert sent["cfg"]["ca"]["url"] == f"https://relay.example.com/ca/{sha_hex}.pem"


def test_admin_push_ca_endpoint_unpin(admin_headers: dict[str, str], broker: FakeBrokerClient):
    _make_user("student-ca-2", "student-ca-2")
    _make_pager_device("pgr-adminca-2", "student-ca-2")

    with _ca_client(broker) as client:
        resp = client.post(
            "/api/admin/devices/pgr-adminca-2/ca", json={"action": "unpin"}, headers=admin_headers
        )

    assert resp.status_code == 200, resp.text
    assert resp.json() == {"ok": True}
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"]["ca"] == {"url": ""}


def test_admin_push_ca_no_ca_configured_is_400(admin_headers: dict[str, str], broker: FakeBrokerClient):
    _make_user("student-ca-3", "student-ca-3")
    _make_pager_device("pgr-adminca-3", "student-ca-3")

    with _ca_client(broker, public_base_url="https://relay.example.com") as client:
        resp = client.post(
            "/api/admin/devices/pgr-adminca-3/ca", json={"action": "push"}, headers=admin_headers
        )
    assert resp.status_code == 400


def test_admin_push_ca_without_public_base_url_is_500(
    admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _make_user("student-ca-4", "student-ca-4")
    _make_pager_device("pgr-adminca-4", "student-ca-4")

    with _ca_client(broker, broker_ca_pem=_CA_PEM) as client:
        resp = client.post(
            "/api/admin/devices/pgr-adminca-4/ca", json={"action": "push"}, headers=admin_headers
        )
    assert resp.status_code == 500


def test_admin_push_ca_unknown_device_404(admin_headers: dict[str, str], broker: FakeBrokerClient):
    with _ca_client(broker) as client:
        resp = client.post(
            "/api/admin/devices/no-such-device/ca", json={"action": "unpin"}, headers=admin_headers
        )
    assert resp.status_code == 404


def test_admin_push_ca_requires_admin(broker: FakeBrokerClient):
    fb_auth.create_user(uid="nonadmin-ca", email="nonadmin-ca@example.com")
    users_store.create_user(uid="nonadmin-ca", alias="nonadminca", display_name="NA")
    with _ca_client(broker) as client:
        resp = client.post(
            "/api/admin/devices/pgr-b-x/ca",
            json={"action": "unpin"},
            headers=auth_header("nonadmin-ca"),
        )
    assert resp.status_code == 403


def test_approve_contact_publishes_book(
    client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _make_user("student20", "student20")
    _make_pager_device("pgr-b-20", "student20")
    ingest = Ingest(broker)
    ingest.handle_up(
        up_topic("pgr-b-20"),
        json.dumps(
            {
                "v": 1,
                "id": "u_b20",
                "ts": int(time.time()),
                "kind": "contact_req",
                "name": "Grandma",
                "ph": "+15550009999",
                "ack": None,
            },
            separators=(",", ":"),
        ).encode("utf-8"),
    )
    broker.clear()

    key = contacts_store.key("pgr-b-20", "u_b20")
    resp = client.post(
        f"/api/admin/contacts/{key}/approve",
        json={"mode": "create", "alias": "grandma20"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    books = [
        json.loads(p.payload) for p in broker.published if json.loads(p.payload).get("kind") == "book"
    ]
    assert len(books) == 1
    assert books[0]["bv"] == 1
    assert books[0]["c"] == [{"a": "grandma20", "n": "Grandma", "t": "sms"}]


def test_reject_contact_publishes_book(
    client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _make_user("student21", "student21")
    _make_pager_device("pgr-b-21", "student21")
    ingest = Ingest(broker)
    ingest.handle_up(
        up_topic("pgr-b-21"),
        json.dumps(
            {
                "v": 1,
                "id": "u_b21",
                "ts": int(time.time()),
                "kind": "contact_req",
                "name": "Stranger",
                "ph": "+15550001234",
                "ack": None,
            },
            separators=(",", ":"),
        ).encode("utf-8"),
    )
    broker.clear()

    key = contacts_store.key("pgr-b-21", "u_b21")
    resp = client.post(
        f"/api/admin/contacts/{key}/reject",
        json={"reason": "not_allowed"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    books = [
        json.loads(p.payload) for p in broker.published if json.loads(p.payload).get("kind") == "book"
    ]
    assert len(books) == 1
    assert books[0]["p"] == [{"n": "Stranger", "s": "no"}]

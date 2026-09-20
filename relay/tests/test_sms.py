"""Device-direct SMS (docs/V02_DESIGN.md §6/§7): `kind:"sms_log"` ingest,
`devices/{id}/smsLog/*` storage, `devices/{id}.smsContacts` + `cfg.sms`
push/pending, and the owner-facing `/api/devices/*` routes
(`app/routers/devices.py`).

Style split, same as `tests/test_contacts.py`: ingest-level tests exercise
`app.ingest.Ingest.handle_up` directly; API-level tests go through the real
FastAPI app so auth/validation are exercised too.
"""

from __future__ import annotations

import json
import time
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import devauth, devcfg
from app.config import Settings
from app.db.firestore import get_db
from app.ingest import Ingest
from app.main import create_app
from app.store import backends as backends_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import sms as sms_store
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


def _ingest_with_broker() -> tuple[Ingest, FakeBrokerClient]:
    broker = FakeBrokerClient()
    return Ingest(broker), broker


def sms_log_payload(
    log_id: str,
    *,
    peer: str = "+15551234567",
    dir_: str = "out",
    st: str = "sent",
    body: str = "hi",
    sms_ts: int | None = None,
    ts: int | None = None,
) -> bytes:
    ts = ts if ts is not None else int(time.time())
    sms_ts = sms_ts if sms_ts is not None else ts
    obj = {
        "v": 1,
        "id": log_id,
        "ts": ts,
        "kind": "sms_log",
        "peer": peer,
        "dir": dir_,
        "st": st,
        "body": body,
        "sms_ts": sms_ts,
    }
    return json.dumps(obj, separators=(",", ":")).encode("utf-8")


def _raw_device(device_id: str) -> dict:
    return get_db().collection("devices").document(device_id).get().to_dict() or {}


# ---------------------------------------------------------------------------
# ingest: kind:"sms_log" (docs/V02_DESIGN.md §6/§7)
# ---------------------------------------------------------------------------


def test_sms_log_out_sent_is_stored():
    _make_user("student", "student")
    _make_pager_device("pgr-s-1", "student")
    ingest, broker = _ingest_with_broker()

    ingest.handle_up(
        up_topic("pgr-s-1"),
        sms_log_payload("s_00000001", peer="+15551234567", dir_="out", st="sent", body="on my way"),
    )

    entries = sms_store.list_log("pgr-s-1")
    assert len(entries) == 1
    assert entries[0].id == "s_00000001"
    assert entries[0].dir == "out"
    assert entries[0].st == "sent"
    assert entries[0].peer == "+15551234567"
    assert entries[0].body == "on my way"
    # Not routed to anyone, not a thread entry -- nothing published.
    assert broker.published == []


def test_sms_log_in_recv_is_stored():
    _make_user("student", "student")
    _make_pager_device("pgr-s-2", "student")
    ingest, _broker = _ingest_with_broker()

    ingest.handle_up(
        up_topic("pgr-s-2"),
        sms_log_payload("s_00000002", peer="+15559990000", dir_="in", st="recv", body="ok"),
    )

    entries = sms_store.list_log("pgr-s-2")
    assert len(entries) == 1
    assert entries[0].dir == "in"
    assert entries[0].st == "recv"


def test_sms_log_blocked_logs_security(caplog: pytest.LogCaptureFixture):
    _make_user("student", "student")
    _make_pager_device("pgr-s-3", "student")
    ingest, _broker = _ingest_with_broker()

    with caplog.at_level("WARNING", logger="relay.ingest"):
        ingest.handle_up(
            up_topic("pgr-s-3"),
            sms_log_payload(
                "s_00000003", peer="+15558887777", dir_="in", st="blocked", body="who is this"
            ),
        )

    entries = sms_store.list_log("pgr-s-3")
    assert entries[0].st == "blocked"
    assert any(
        "SECURITY sms-blocked device=pgr-s-3 peer=+15558887777" in rec.message
        for rec in caplog.records
    )


def test_sms_log_empty_body_is_allowed():
    _make_user("student", "student")
    _make_pager_device("pgr-s-4", "student")
    ingest, _broker = _ingest_with_broker()

    ingest.handle_up(up_topic("pgr-s-4"), sms_log_payload("s_00000004", body=""))

    entries = sms_store.list_log("pgr-s-4")
    assert entries[0].body == ""


def test_sms_log_dedup_by_id_is_idempotent():
    _make_user("student", "student")
    _make_pager_device("pgr-s-5", "student")
    ingest, _broker = _ingest_with_broker()

    payload = sms_log_payload("s_00000005", body="once")
    ingest.handle_up(up_topic("pgr-s-5"), payload)
    ingest.handle_up(up_topic("pgr-s-5"), payload)  # webhook redelivery

    assert len(sms_store.list_log("pgr-s-5")) == 1


def test_sms_log_from_unregistered_device_dropped():
    ingest, _broker = _ingest_with_broker()
    ingest.handle_up(up_topic("pgr-s-missing"), sms_log_payload("s_00000006"))
    assert sms_store.list_log("pgr-s-missing") == []


def test_sms_log_from_revoked_device_dropped():
    _make_user("student", "student")
    _make_pager_device("pgr-s-6", "student")
    devices_store.revoke_device("pgr-s-6")
    ingest, _broker = _ingest_with_broker()

    ingest.handle_up(up_topic("pgr-s-6"), sms_log_payload("s_00000007"))
    assert sms_store.list_log("pgr-s-6") == []


@pytest.mark.parametrize(
    "kwargs",
    [
        {"peer": "not-a-phone"},
        {"dir_": "sideways"},
        {"st": "delivered"},
        {"body": "x" * 161},
    ],
)
def test_sms_log_malformed_is_dropped(kwargs: dict[str, str]):
    _make_user("student", "student")
    _make_pager_device("pgr-s-7", "student")
    ingest, _broker = _ingest_with_broker()

    ingest.handle_up(up_topic("pgr-s-7"), sms_log_payload("s_00000008", **kwargs))
    assert sms_store.list_log("pgr-s-7") == []


def test_hmac_signed_sms_log_is_verified_then_stored():
    _make_user("student", "student")
    key = _make_hmac_pager_device("pgr-s-8", "student")
    ingest, _broker = _ingest_with_broker()

    topic = up_topic("pgr-s-8")
    payload = devauth.sign_json(
        key,
        topic,
        {
            "v": 1,
            "id": "s_00000009",
            "ts": int(time.time()),
            "kind": "sms_log",
            "peer": "+15551112222",
            "dir": "out",
            "st": "sent",
            "body": "signed",
            "sms_ts": int(time.time()),
            "n": 1,
        },
    )
    ingest.handle_up(topic, payload)

    entries = sms_store.list_log("pgr-s-8")
    assert len(entries) == 1
    assert entries[0].peer == "+15551112222"


def test_hmac_signed_sms_log_bad_signature_dropped():
    _make_user("student", "student")
    _make_hmac_pager_device("pgr-s-9", "student")
    ingest, _broker = _ingest_with_broker()

    topic = up_topic("pgr-s-9")
    payload = devauth.sign_json(
        b"wrong-key-" + b"0" * 22,
        topic,
        {
            "v": 1,
            "id": "s_0000000a",
            "ts": int(time.time()),
            "kind": "sms_log",
            "peer": "+15551112222",
            "dir": "out",
            "st": "sent",
            "body": "bad sig",
            "sms_ts": int(time.time()),
            "n": 1,
        },
    )
    ingest.handle_up(topic, payload)
    assert sms_store.list_log("pgr-s-9") == []


def test_hmac_signed_sms_log_replay_dropped():
    _make_user("student", "student")
    key = _make_hmac_pager_device("pgr-s-10", "student")
    ingest, _broker = _ingest_with_broker()
    topic = up_topic("pgr-s-10")

    def _payload(log_id: str, n: int) -> bytes:
        return devauth.sign_json(
            key,
            topic,
            {
                "v": 1,
                "id": log_id,
                "ts": int(time.time()),
                "kind": "sms_log",
                "peer": "+15551112222",
                "dir": "out",
                "st": "sent",
                "body": "x",
                "sms_ts": int(time.time()),
                "n": n,
            },
        )

    ingest.handle_up(topic, _payload("s_0000000b", 5))
    ingest.handle_up(topic, _payload("s_0000000c", 5))  # replayed n
    assert len(sms_store.list_log("pgr-s-10")) == 1


# ---------------------------------------------------------------------------
# app.store.sms -- collection primitive
# ---------------------------------------------------------------------------


def test_store_sms_create_log_dedup_and_list_order():
    now = int(time.time())
    assert sms_store.create_log(
        "pgr-store-1", "s_1", ts=now, sms_ts=now, dir_="out", peer="+15550000001", st="sent", body="a"
    )
    assert sms_store.create_log(
        "pgr-store-1", "s_2", ts=now + 1, sms_ts=now + 1, dir_="in", peer="+15550000002", st="recv", body="b"
    )
    # Redelivery of the same id is a no-op.
    assert not sms_store.create_log(
        "pgr-store-1", "s_1", ts=now, sms_ts=now, dir_="out", peer="+15550000001", st="sent", body="a"
    )

    entries = sms_store.list_log("pgr-store-1")
    assert [e.id for e in entries] == ["s_2", "s_1"]  # newest first


def test_store_sms_list_log_before_cursor_and_limit():
    base = int(time.time())
    for i in range(5):
        sms_store.create_log(
            "pgr-store-2",
            f"s_{i}",
            ts=base + i,
            sms_ts=base + i,
            dir_="out",
            peer="+15550000000",
            st="sent",
            body=str(i),
        )
    newest_three = sms_store.list_log("pgr-store-2", limit=3)
    assert [e.id for e in newest_three] == ["s_4", "s_3", "s_2"]

    before_cursor = sms_store.list_log("pgr-store-2", before=base + 3)
    assert [e.id for e in before_cursor] == ["s_2", "s_1", "s_0"]


# ---------------------------------------------------------------------------
# app.devcfg: push_sms_contacts / sms_pending
# ---------------------------------------------------------------------------


def test_push_sms_contacts_publishes_and_stores_pending():
    _make_user("student11", "student11")
    _make_pager_device("pgr-cfg-1", "student11")
    broker = FakeBrokerClient()

    ok = devcfg.push_sms_contacts(
        "pgr-cfg-1", [{"name": "Mom", "phone": "+12065550100"}], broker
    )

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    assert sent["cfg"] == {"sms": [{"n": "Mom", "p": "+12065550100"}]}
    assert sent["ack"] is None

    raw = _raw_device("pgr-cfg-1")
    assert raw["pendingCfgSms"]["id"] == sent["id"]
    assert raw["pendingCfgSms"]["acked"] is False
    assert devcfg.sms_pending("pgr-cfg-1") is True


def test_push_sms_contacts_empty_list_is_valid():
    _make_user("student12", "student12")
    _make_pager_device("pgr-cfg-2", "student12")
    broker = FakeBrokerClient()

    ok = devcfg.push_sms_contacts("pgr-cfg-2", [], broker)

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"] == {"sms": []}


def test_push_sms_contacts_unregistered_device_returns_false():
    broker = FakeBrokerClient()
    assert devcfg.push_sms_contacts("no-such-device", [], broker) is False
    assert broker.published == []


def test_sms_pending_false_before_any_push_and_after_ack():
    _make_user("student13", "student13")
    _make_pager_device("pgr-cfg-3", "student13")
    broker = FakeBrokerClient()
    assert devcfg.sms_pending("pgr-cfg-3") is False

    devcfg.push_sms_contacts("pgr-cfg-3", [{"name": "Dad", "phone": "+12065550101"}], broker)
    assert devcfg.sms_pending("pgr-cfg-3") is True
    sent_id = json.loads(broker.published[0].payload)["id"]

    assert devcfg.ack("pgr-cfg-3", sent_id) is True
    assert devcfg.sms_pending("pgr-cfg-3") is False


def test_pending_cfg_sms_independent_of_pending_lock_and_ca():
    """docs/V02_DESIGN.md §6: `cfg.sms` gets its own pending slot, never
    clobbering (or clobbered by) a pending `cfg.lock`/`cfg.ca`."""
    _make_user("student14", "student14")
    _make_pager_device("pgr-cfg-4", "student14")
    broker = FakeBrokerClient()

    devcfg.push_cfg("pgr-cfg-4", {"auto": 5}, broker)
    devcfg.push_sms_contacts(
        "pgr-cfg-4", [{"name": "Mom", "phone": "+12065550100"}], broker
    )
    lock_id = _raw_device("pgr-cfg-4")["pendingCfg"]["id"]
    sms_id = _raw_device("pgr-cfg-4")["pendingCfgSms"]["id"]
    assert lock_id != sms_id
    broker.clear()

    devcfg.republish_pending("pgr-cfg-4", broker)
    assert len(broker.published) == 2
    sent_ids = {json.loads(p.payload)["id"] for p in broker.published}
    assert sent_ids == {lock_id, sms_id}

    assert devcfg.ack("pgr-cfg-4", sms_id) is True
    assert _raw_device("pgr-cfg-4")["pendingCfg"]["acked"] is False
    assert devcfg.ack("pgr-cfg-4", lock_id) is True
    assert _raw_device("pgr-cfg-4")["pendingCfgSms"]["acked"] is True


def test_push_sms_contacts_eight_worst_case_entries_fits_both_encodings():
    """docs/V02_DESIGN.md §6/§7: "check the 640-byte envelope limit with 8
    worst-case entries in both encodings" -- this is that check, run as a
    regression test rather than only a one-off calculation in the task
    report. Name at `SMS_CONTACT_NAME_MAX_UTF8_BYTES` (24) worst-case UTF-8
    bytes packed into 16 code points (12 two-byte characters), phone at its
    E.164 maximum length."""
    _make_user("student15", "student15")
    _make_pager_device("pgr-cfg-5", "student15")
    broker = FakeBrokerClient()
    contacts = [
        {"name": "é" * 12, "phone": "+" + "9" * 15} for _ in range(devices_store.MAX_SMS_CONTACTS)
    ]
    assert len("é" * 12) <= devices_store.SMS_CONTACT_NAME_MAX_CODEPOINTS
    assert len(("é" * 12).encode("utf-8")) == devices_store.SMS_CONTACT_NAME_MAX_UTF8_BYTES

    ok = devcfg.push_sms_contacts("pgr-cfg-5", contacts, broker)
    assert ok is True  # would have raised AssertionError from
    # `_assert_within_envelope_limit` if either encoding overflowed 640 bytes.


# ---------------------------------------------------------------------------
# API: /api/devices, /api/devices/{id}/sms-contacts, /api/devices/{id}/sms-log
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
    auth_user = fb_auth.create_user(email="sms-admin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="smsadmin", display_name="Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    return auth_header(auth_user.uid)


def test_get_api_devices_lists_only_own_devices(client: TestClient):
    _make_user("owner1", "owner1")
    _make_user("owner2", "owner2")
    _make_pager_device("pgr-api-1", "owner1")
    _make_pager_device("pgr-api-2", "owner2")

    resp = client.get("/api/devices", headers=auth_header("owner1"))
    assert resp.status_code == 200, resp.text
    ids = [d["id"] for d in resp.json()]
    assert ids == ["pgr-api-1"]


def test_get_sms_contacts_owner_allowed(client: TestClient):
    _make_user("owner3", "owner3")
    _make_pager_device("pgr-api-3", "owner3")

    resp = client.get("/api/devices/pgr-api-3/sms-contacts", headers=auth_header("owner3"))
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"contacts": [], "pending": False}


def test_get_sms_contacts_third_party_forbidden(client: TestClient):
    _make_user("owner4", "owner4")
    _make_user("stranger4", "stranger4")
    _make_pager_device("pgr-api-4", "owner4")

    resp = client.get("/api/devices/pgr-api-4/sms-contacts", headers=auth_header("stranger4"))
    assert resp.status_code == 403


def test_get_sms_contacts_admin_allowed(client: TestClient, admin_headers: dict[str, str]):
    _make_user("owner5", "owner5")
    _make_pager_device("pgr-api-5", "owner5")

    resp = client.get("/api/devices/pgr-api-5/sms-contacts", headers=admin_headers)
    assert resp.status_code == 200, resp.text


def test_get_sms_contacts_unknown_device_404(client: TestClient):
    _make_user("owner6", "owner6")
    resp = client.get("/api/devices/no-such-device/sms-contacts", headers=auth_header("owner6"))
    assert resp.status_code == 404


def test_put_sms_contacts_stores_pushes_and_returns_shape(
    client: TestClient, broker: FakeBrokerClient
):
    _make_user("owner7", "owner7")
    _make_pager_device("pgr-api-7", "owner7")

    resp = client.put(
        "/api/devices/pgr-api-7/sms-contacts",
        json={"contacts": [{"name": "Mom", "phone": "+12065550100"}]},
        headers=auth_header("owner7"),
    )
    assert resp.status_code == 200, resp.text
    assert resp.json() == {
        "contacts": [{"name": "Mom", "phone": "+12065550100"}],
        "pending": True,
    }
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"]["sms"] == [{"n": "Mom", "p": "+12065550100"}]

    # And it landed on the device doc itself.
    assert devices_store.get_device("pgr-api-7").smsContacts[0].name == "Mom"


def test_put_sms_contacts_forbidden_for_non_owner(client: TestClient):
    _make_user("owner8", "owner8")
    _make_user("stranger8", "stranger8")
    _make_pager_device("pgr-api-8", "owner8")

    resp = client.put(
        "/api/devices/pgr-api-8/sms-contacts",
        json={"contacts": []},
        headers=auth_header("stranger8"),
    )
    assert resp.status_code == 403


def test_put_sms_contacts_over_max_is_422(client: TestClient):
    _make_user("owner9", "owner9")
    _make_pager_device("pgr-api-9", "owner9")

    contacts = [{"name": f"c{i}", "phone": f"+1206555010{i}"} for i in range(9)]
    resp = client.put(
        "/api/devices/pgr-api-9/sms-contacts",
        json={"contacts": contacts},
        headers=auth_header("owner9"),
    )
    assert resp.status_code == 422
    assert "at most 8" in resp.text


def test_put_sms_contacts_duplicate_phone_is_422(client: TestClient):
    _make_user("owner10", "owner10")
    _make_pager_device("pgr-api-10", "owner10")

    resp = client.put(
        "/api/devices/pgr-api-10/sms-contacts",
        json={
            "contacts": [
                {"name": "Mom", "phone": "+12065550100"},
                {"name": "Mom Cell", "phone": "+12065550100"},
            ]
        },
        headers=auth_header("owner10"),
    )
    assert resp.status_code == 422
    assert "unique" in resp.text


def test_put_sms_contacts_bad_phone_is_422_naming_the_entry(client: TestClient):
    _make_user("owner11", "owner11")
    _make_pager_device("pgr-api-11", "owner11")

    resp = client.put(
        "/api/devices/pgr-api-11/sms-contacts",
        json={"contacts": [{"name": "Bad", "phone": "0000"}]},
        headers=auth_header("owner11"),
    )
    assert resp.status_code == 422
    assert "0000" in resp.text


def test_put_sms_contacts_name_too_long_is_422(client: TestClient):
    _make_user("owner12", "owner12")
    _make_pager_device("pgr-api-12", "owner12")

    resp = client.put(
        "/api/devices/pgr-api-12/sms-contacts",
        json={"contacts": [{"name": "N" * 17, "phone": "+12065550100"}]},
        headers=auth_header("owner12"),
    )
    assert resp.status_code == 422


def test_get_sms_log_resolves_name_and_orders_newest_first(client: TestClient):
    _make_user("owner13", "owner13")
    _make_pager_device("pgr-api-13", "owner13")
    devices_store.set_sms_contacts(
        "pgr-api-13", [devices_store.SmsContact(name="Mom", phone="+12065550100")]
    )
    now = int(time.time())
    sms_store.create_log(
        "pgr-api-13", "s_a", ts=now, sms_ts=now, dir_="out", peer="+12065550100", st="sent", body="hi"
    )
    sms_store.create_log(
        "pgr-api-13",
        "s_b",
        ts=now + 1,
        sms_ts=now + 1,
        dir_="in",
        peer="+19998887777",
        st="blocked",
        body="who",
    )

    resp = client.get("/api/devices/pgr-api-13/sms-log", headers=auth_header("owner13"))
    assert resp.status_code == 200, resp.text
    entries = resp.json()["entries"]
    assert [e["id"] for e in entries] == ["s_b", "s_a"]
    assert entries[1]["name"] == "Mom"
    assert entries[0]["name"] is None


def test_get_sms_log_forbidden_for_non_owner(client: TestClient):
    _make_user("owner14", "owner14")
    _make_user("stranger14", "stranger14")
    _make_pager_device("pgr-api-14", "owner14")

    resp = client.get("/api/devices/pgr-api-14/sms-log", headers=auth_header("stranger14"))
    assert resp.status_code == 403


def test_get_sms_log_unknown_device_404(client: TestClient):
    _make_user("owner15", "owner15")
    resp = client.get("/api/devices/no-such-device/sms-log", headers=auth_header("owner15"))
    assert resp.status_code == 404


def test_get_sms_log_limit_bounds_are_422(client: TestClient):
    _make_user("owner16", "owner16")
    _make_pager_device("pgr-api-16", "owner16")

    resp = client.get(
        "/api/devices/pgr-api-16/sms-log?limit=501", headers=auth_header("owner16")
    )
    assert resp.status_code == 422

    resp2 = client.get(
        "/api/devices/pgr-api-16/sms-log?limit=0", headers=auth_header("owner16")
    )
    assert resp2.status_code == 422

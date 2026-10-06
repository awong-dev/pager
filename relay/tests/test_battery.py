"""docs/BATTERY_STATS_DESIGN.md B5/B6: `/status` `bs` -> `devices/{id}/battery`,
`GET /api/devices/{id}/battery`, `PUT /api/admin/battery-models/{id}`, retention."""

from __future__ import annotations

import time
from collections.abc import Iterator
from datetime import UTC, datetime, timedelta

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import battmodel, devauth, jobs
from app.config import Settings
from app.db.firestore import get_db
from app.ingest import Ingest
from app.main import create_app
from app.routing import Routing
from app.store import devices as devices_store
from app.store import users as users_store
from tests.conftest import offline_status_payload, status_topic
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header
from tests.test_ingest import _HMAC_KEY, _make_hmac_pager_device, _make_user

BS = {
    "sq": 7, "dt": 3600, "sl": 3530, "aw": [60, 1, 2, 10, 3, 4], "ns": 180, "x1": 2,
    "rl": 10, "rf": [1, 2, 3], "mvn": 3790, "cn": 1, "md": [5, 6, 7], "re": 13,
}


def _status(session: str = "s_00000001", **extra) -> dict:
    return {
        "v": 1, "state": "online", "mode": "sleep", "batt_mv": 3800, "rssi": -90,
        "session": session, "ts": int(time.time()), "fw": "0.1.0", "link": 2, **extra,
    }


def _send(ingest: Ingest, device_id: str, obj: dict, n: int) -> None:
    topic = status_topic(device_id)
    ingest.handle_status(topic, devauth.sign_cbor(_HMAC_KEY, topic, {**obj, "n": n}))


@pytest.fixture
def ingest() -> Ingest:
    broker = FakeBrokerClient()
    return Ingest(broker, Routing(broker))


@pytest.fixture
def dev(ingest: Ingest) -> str:
    _make_user("student", "student")
    _make_hmac_pager_device("d", "student")
    return "d"


def _docs(device_id: str = "d") -> list:
    return list(get_db().collection("devices").document(device_id).collection("battery").stream())


def test_status_with_bs_writes_every_field(ingest, dev):
    _send(ingest, dev, _status(bs=BS), 1)
    docs = _docs()
    assert [d.id for d in docs] == ["s_00000001-7"]
    d = docs[0].to_dict()
    assert d["hasBs"] is True and d["session"] == "s_00000001" and d["battMv"] == 3800
    assert d["sq"] == 7 and d["minMv"] == 3790 and d["dtS"] == 3600 and d["sleepS"] == 3530
    assert d["awakeS"] == {"timer": 60, "attn": 1, "hot": 2, "ui": 10, "modem": 3, "fetch": 4}
    assert d["sleeps"] == 180 and d["ext1"] == 2 and d["railS"] == 10
    assert d["refresh"] == {"full": 1, "partial": 2, "upgraded": 3}
    assert d["connects"] == 1 and d["modemS"] == {"off": 5, "search": 6, "gnss": 7}
    assert d["radioEvents"] == 13 and d["rssi"] == -90 and d["mode"] == "sleep"
    assert d["fw"] == "0.1.0" and d["link"] == 2 and "createdAt" in d and d["ts"] > 0


def test_same_status_twice_is_one_doc(ingest, dev):
    _send(ingest, dev, _status(bs=BS), 1)
    _send(ingest, dev, _status(bs={**BS, "dt": 3700}), 2)
    docs = _docs()
    assert len(docs) == 1 and docs[0].to_dict()["dtS"] == 3700


def test_short_aw_keeps_status_and_sample_without_bs(ingest, dev):
    _send(ingest, dev, _status(bs={**BS, "aw": [1, 2]}), 1)
    docs = _docs()
    assert len(docs) == 1
    d = docs[0].to_dict()
    assert d["hasBs"] is False and d["battMv"] == 3800 and "awakeS" not in d
    assert devices_store.get_device(dev).status.battMv == 3800


def test_status_without_bs_writes_auto_id_sample(ingest, dev):
    _send(ingest, dev, _status(), 1)
    docs = _docs()
    assert len(docs) == 1 and docs[0].id != "s_00000001-0"
    assert docs[0].to_dict()["battMv"] == 3800 and docs[0].to_dict()["hasBs"] is False


def test_lwt_writes_nothing(ingest, dev):
    ingest.handle_status(status_topic(dev), offline_status_payload("s_00000001"))
    assert _docs() == []


# ---- API ----


def _settings() -> Settings:
    return Settings(
        broker_api_url="http://unused.invalid/api/v5", broker_api_key=None,
        broker_api_secret=None, webhook_key="k", dev_mode=True, google_cloud_project=None,
        firestore_emulator_host=None, firebase_auth_emulator_host=None,
    )


@pytest.fixture
def client() -> Iterator[TestClient]:
    with TestClient(create_app(settings=_settings(), broker_client=FakeBrokerClient())) as c:
        yield c


def _user(email: str, role: str = "member") -> dict[str, str]:
    u = fb_auth.create_user(email=email)
    users_store.create_user(uid=u.uid, alias=email.split("@")[0], display_name="x", role=role,
                            family_id="default")
    fb_auth.set_custom_user_claims(u.uid, {"role": role, "fam": "default"})
    return auth_header(u.uid)


def _owned_device(owner_headers: dict[str, str]) -> str:
    uid = fb_auth.verify_id_token(owner_headers["Authorization"].split()[1])["uid"]
    devices_store.create_device(device_id="d", owner_uid=uid, label="d", mqtt_username="d",
                                mqtt_password_hash="x", auth_mode="password")
    return "d"


def _put_sample(ts: int, created_days_ago: float, doc_id: str) -> None:
    get_db().collection("devices").document("d").collection("battery").document(doc_id).set(
        {"ts": ts, "battMv": 3800, "hasBs": False,
         "createdAt": datetime.now(UTC) - timedelta(days=created_days_ago)}
    )


def test_get_battery_authz_and_span(client):
    owner = _user("owner@example.com")
    other = _user("other@example.com")
    _owned_device(owner)
    now = int(time.time())
    _put_sample(now - 100, 0, "a")
    _put_sample(now - 50, 0, "b")
    r = client.get("/api/devices/d/battery", headers=owner)
    assert r.status_code == 200
    body = r.json()
    assert [s["id"] for s in body["samples"]] == ["a", "b"]
    assert "createdAt" not in body["samples"][0] and body["truncated"] is False
    assert body["model"]["source"] == "prior" and body["deviceId"] == "d"
    assert client.get("/api/devices/d/battery", headers=other).status_code == 403
    assert client.get(f"/api/devices/d/battery?since=0&until={91 * 86400}",
                      headers=owner).status_code == 422
    assert client.get("/api/devices/d/battery?since=10&until=5", headers=owner).status_code == 422


def test_model_priors_then_super_put(client):
    owner = _user("owner@example.com")
    sup = _user("root@example.com", "super")
    _owned_device(owner)
    m = client.get("/api/devices/d/battery", headers=owner).json()["model"]
    assert m["source"] == "prior" and m["sleep_mA"] == 1.0
    body = battmodel.BatteryModel(sleep_mA=1.5, source="fit", fitAt=123, notes="k=1.2").model_dump()
    assert client.put("/api/admin/battery-models/walter-6092-lipo2500", json=body,
                      headers=owner).status_code == 403
    r = client.put("/api/admin/battery-models/walter-6092-lipo2500", json=body, headers=sup)
    assert r.status_code == 200
    m = client.get("/api/devices/d/battery", headers=owner).json()["model"]
    assert m["source"] == "fit" and m["sleep_mA"] == 1.5 and m["fitAt"] == 123
    assert m["awake_mA"] == body["awake_mA"]
    assert client.put("/api/admin/battery-models/BAD_ID", json=body, headers=sup).status_code == 422
    assert client.put("/api/admin/battery-models/x", json={**body, "sleep_mA": 600},
                      headers=sup).status_code == 422


def test_sweep_deletes_old_battery_samples():
    devices_store.create_device(device_id="d", owner_uid="u", label="d", mqtt_username="d",
                                mqtt_password_hash="x", auth_mode="password")
    _put_sample(1, 91, "old")
    _put_sample(2, 1, "new")
    res = jobs.sweep()
    assert res.batteryDeleted == 1
    assert [d.id for d in _docs()] == ["new"]


def test_model_mah_test_vector():
    sample = {
        "hasBs": True, "dtS": 3600, "sleepS": 3530,
        "awakeS": {"timer": 60, "ui": 10}, "railS": 10, "modemS": {"off": 0, "search": 0, "gnss": 0},
        "radioEvents": 13, "connects": 0, "refresh": {"full": 0, "partial": 2, "upgraded": 0},
    }
    m = {**battmodel.PRIORS}
    assert battmodel.model_mah(sample, m) == pytest.approx(3.4242, abs=1e-4)
    assert battmodel.model_mah({"hasBs": False}, m) is None

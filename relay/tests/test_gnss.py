"""docs/GNSS_DISABLE_DESIGN.md: `cfg.loc.gnss` wire shapes, push/ack/pending
(`app/devcfg.py`), and `GET`/`PUT /api/devices/{id}/gnss`. Fixtures mirror
`tests/test_wifi.py`."""

from __future__ import annotations

import json
from collections.abc import Iterator

import cbor2
import pytest
from fastapi.testclient import TestClient

from app import devcfg, wirecbor
from app.main import create_app
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import users as users_store
from app.wire import StatusEnvelope
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header
from tests.test_wifi import make_settings


def _make_user(uid: str) -> None:
    users_store.create_user(uid=uid, alias=uid, display_name=uid)


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


@pytest.fixture
def client(broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        yield c


def test_cfg_loc_cbor_shape_and_round_trip():
    obj = {"v": 1, "id": "m_aabbccdd", "ts": 1_700_000_000, "kind": "cfg",
           "cfg": {"loc": {"gnss": False}}, "ack": None}  # fmt: skip
    raw = wirecbor.encode(obj)
    assert cbor2.loads(raw)[wirecbor.KEYMAP["cfg"]] == {5: {0: False}}
    assert wirecbor.decode(raw)["cfg"] == {"loc": {"gnss": False}}


def test_cfg_loc_alongside_wifi_round_trips():
    obj = {"v": 1, "id": "m_aabbccdd", "ts": 1_700_000_000, "kind": "cfg",
           "cfg": {"wifi": {"en": True}, "loc": {"gnss": True}}, "ack": None}  # fmt: skip
    assert wirecbor.decode(wirecbor.encode(obj))["cfg"] == obj["cfg"]


def test_status_gnss_key_69_round_trips():
    obj = {"v": 1, "state": "online", "mode": "sleep", "batt_mv": 3300, "rssi": -80,
           "session": "s_aabbccdd", "ts": 1_700_000_000, "gnss": 0}  # fmt: skip
    raw = wirecbor.encode(obj)
    assert cbor2.loads(raw)[69] == 0
    assert wirecbor.decode(raw)["gnss"] == 0


@pytest.mark.parametrize("val,expected", [(0, 0), (1, 1), (2, None), (-1, None), (None, None)])
def test_status_gnss_validated(val, expected):
    env = StatusEnvelope.model_validate(
        {"v": 1, "state": "online", "mode": "sleep", "batt_mv": 3300, "rssi": -80,
         "session": "s_aabbccdd", "ts": 1_700_000_000, "gnss": val}
    )
    assert env.gnss == expected


def test_push_loc_publishes_and_pending_flips(broker: FakeBrokerClient):
    _make_user("gowner0")
    _make_pager_device("pgr-gnss-0", "gowner0")
    assert devcfg.loc_pending("pgr-gnss-0") is False

    assert devcfg.push_loc("pgr-gnss-0", gnss=False, broker=broker) is True
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    assert sent["cfg"] == {"loc": {"gnss": False}}
    assert devcfg.loc_pending("pgr-gnss-0") is True

    assert devcfg.ack("pgr-gnss-0", sent["id"]) is True
    assert devcfg.loc_pending("pgr-gnss-0") is False


def test_push_loc_unknown_device(broker: FakeBrokerClient):
    assert devcfg.push_loc("nope", gnss=True, broker=broker) is False
    assert broker.published == []


def test_get_gnss_defaults(client: TestClient):
    _make_user("gowner1")
    _make_pager_device("pgr-gnss-1", "gowner1")
    resp = client.get("/api/devices/pgr-gnss-1/gnss", headers=auth_header("gowner1"))
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"en": True, "pending": False, "reported": None}


def test_gnss_third_party_forbidden(client: TestClient):
    _make_user("gowner2")
    _make_user("gstranger2")
    _make_pager_device("pgr-gnss-2", "gowner2")
    h = auth_header("gstranger2")
    assert client.get("/api/devices/pgr-gnss-2/gnss", headers=h).status_code == 403
    assert client.put("/api/devices/pgr-gnss-2/gnss", json={"en": False}, headers=h).status_code == 403


def test_put_gnss_stores_publishes_and_acks(client: TestClient, broker: FakeBrokerClient):
    _make_user("gowner3")
    _make_pager_device("pgr-gnss-3", "gowner3")
    devices_store.update_status("pgr-gnss-3", gnss=1)
    h = auth_header("gowner3")

    resp = client.put("/api/devices/pgr-gnss-3/gnss", json={"en": False}, headers=h)
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"en": False, "pending": True, "reported": 1}
    assert devices_store.get_gnss_enabled("pgr-gnss-3") is False
    assert len(broker.published) == 1
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"] == {"loc": {"gnss": False}}

    assert devcfg.ack("pgr-gnss-3", sent["id"]) is True
    resp = client.get("/api/devices/pgr-gnss-3/gnss", headers=h)
    assert resp.json() == {"en": False, "pending": False, "reported": 1}


def test_put_gnss_rejects_missing_en(client: TestClient):
    _make_user("gowner4")
    _make_pager_device("pgr-gnss-4", "gowner4")
    resp = client.put("/api/devices/pgr-gnss-4/gnss", json={}, headers=auth_header("gowner4"))
    assert resp.status_code == 422

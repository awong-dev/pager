"""WiFi credentials (docs/WIFI_DESIGN.md §4/§6, docs/WIFI_TASKS.md W7):
`cfg.wifi` push/ack/pending (`app/devcfg.py`), `deviceSecrets/{id}.wifiEnabled`
/`.wifiNets` storage (`app/store/device_secrets.py`), and the owner-facing
`GET`/`PUT /api/devices/{id}/wifi` route (`app/routers/devices.py`).

Style split, same as `tests/test_sms.py`: `app.devcfg`/`app.store.
device_secrets` functions are exercised directly (fast, no HTTP); API-level
tests go through the real FastAPI app so auth/validation/the `tls`-pinned
guard are all exercised too.
"""

from __future__ import annotations

import json
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth
from pydantic import ValidationError

from app import devcfg, wirecbor
from app.config import Settings
from app.db.firestore import get_db
from app.main import create_app
from app.store import backends as backends_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import users as users_store
from app.store.device_secrets import WifiNet
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

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


def _raw_device(device_id: str) -> dict:
    return get_db().collection("devices").document(device_id).get().to_dict() or {}


def _raw_secret(device_id: str) -> dict:
    return get_db().collection("deviceSecrets").document(device_id).get().to_dict() or {}


# ---------------------------------------------------------------------------
# app/store/device_secrets.py: `WifiNet` validators, `get_wifi`/`set_wifi`
# ---------------------------------------------------------------------------


def test_wifi_net_accepts_boundary_lengths():
    WifiNet(s="a", p="p" * 8)  # 1-byte ssid, 8-byte psk (the minima)
    WifiNet(s="s" * 32, p="p" * 63)  # 32-byte ssid, 63-byte psk (the maxima)


@pytest.mark.parametrize("ssid", ["", "s" * 33])
def test_wifi_net_rejects_ssid_out_of_range(ssid: str):
    with pytest.raises(ValidationError):
        WifiNet(s=ssid, p="p" * 8)


@pytest.mark.parametrize("psk", ["p" * 7, "p" * 64])
def test_wifi_net_rejects_psk_out_of_range(psk: str):
    with pytest.raises(ValidationError):
        WifiNet(s="ssid", p=psk)


def test_wifi_net_rejects_control_characters():
    with pytest.raises(ValidationError):
        WifiNet(s="ss\x00id", p="p" * 8)
    with pytest.raises(ValidationError):
        WifiNet(s="ssid", p="pass\x01word")


def test_get_wifi_defaults_to_disabled_and_empty_for_unknown_device():
    en, nets = device_secrets_store.get_wifi("no-such-device")
    assert en is False
    assert nets == []


def test_set_wifi_en_only_leaves_stored_nets_untouched():
    device_secrets_store.set_wifi(
        "sec-1", en=True, nets=[WifiNet(s="home", p="hunter22")]
    )
    device_secrets_store.set_wifi("sec-1", en=False, nets=None)

    en, nets = device_secrets_store.get_wifi("sec-1")
    assert en is False
    assert [n.s for n in nets] == ["home"]


def test_set_wifi_empty_list_clears_stored_nets():
    device_secrets_store.set_wifi(
        "sec-2", en=True, nets=[WifiNet(s="home", p="hunter22")]
    )
    device_secrets_store.set_wifi("sec-2", en=True, nets=[])

    en, nets = device_secrets_store.get_wifi("sec-2")
    assert en is True
    assert nets == []


def test_wifi_credentials_are_never_stored_on_the_devices_document():
    """docs/WIFI_TASKS.md W7 (coordinator note): a PSK is a credential like
    `hmacKey`/`mqttPasswordHash`, so it lives in `deviceSecrets/{d}` (default-
    deny for every client, `app/store/device_secrets.py`'s own docstring),
    never on `devices/{d}` (readable by the device's own owner)."""
    _make_user("wifisecuser", "wifisecuser")
    _make_pager_device("pgr-wifi-sec-1", "wifisecuser")

    device_secrets_store.set_wifi(
        "pgr-wifi-sec-1", en=True, nets=[WifiNet(s="home", p="hunter22")]
    )

    raw_device = _raw_device("pgr-wifi-sec-1")
    assert "wifiEnabled" not in raw_device
    assert "wifiNets" not in raw_device
    raw_secret = _raw_secret("pgr-wifi-sec-1")
    assert raw_secret["wifiEnabled"] is True
    assert raw_secret["wifiNets"] == [{"s": "home", "p": "hunter22"}]


# ---------------------------------------------------------------------------
# app/wirecbor.py: `cfg.wifi` keymap (key 3) / `wifi.nets[]` sub-map,
# round-tripped through both encodings.
# ---------------------------------------------------------------------------


def test_cfg_wifi_keymap_allocation():
    assert wirecbor.CFG_KEYMAP["wifi"] == 3
    assert wirecbor.WIFI_KEYMAP == {"en": 0, "nets": 1}
    assert wirecbor.WIFI_NET_KEYMAP == {"s": 0, "p": 1}
    assert wirecbor.KEYMAP["xport"] == 52


def test_cfg_wifi_round_trips_through_cbor_with_nets():
    obj = {
        "v": 1,
        "id": "m_aaaaaaaa",
        "ts": 1_700_000_000,
        "kind": "cfg",
        "cfg": {"wifi": {"en": True, "nets": [{"s": "home", "p": "hunter22"}]}},
        "ack": None,
    }
    encoded = wirecbor.encode(obj)
    decoded = wirecbor.decode(encoded)
    assert decoded["cfg"] == {"wifi": {"en": True, "nets": [{"s": "home", "p": "hunter22"}]}}


def test_cfg_wifi_round_trips_through_cbor_en_only_no_nets_key():
    """`nets` absent on the wire means "leave the stored networks alone" --
    this asserts the *key itself* is missing after a round trip, not merely
    an empty list, since those two mean different things (docs/WIFI_DESIGN.md
    §4)."""
    obj = {
        "v": 1,
        "id": "m_aaaaaaaa",
        "ts": 1_700_000_000,
        "kind": "cfg",
        "cfg": {"wifi": {"en": False}},
        "ack": None,
    }
    encoded = wirecbor.encode(obj)
    decoded = wirecbor.decode(encoded)
    assert decoded["cfg"] == {"wifi": {"en": False}}
    assert "nets" not in decoded["cfg"]["wifi"]


def test_cfg_wifi_two_maximal_entries_fit_640_byte_cap_both_encodings():
    """docs/WIFI_TASKS.md W7 Verify: "the cfg.wifi round-trips through
    wirecbor under the 640 B cap with 2 maximal entries." SSID at its
    32-byte maximum, PSK at its 63-byte maximum, both networks -- the
    heaviest `cfg.wifi` push that can exist."""
    obj = {
        "v": 1,
        "id": "m" * 16,
        "ts": 1_700_000_000,
        "kind": "cfg",
        "cfg": {
            "wifi": {
                "en": True,
                "nets": [
                    {"s": "s" * 32, "p": "p" * 63},
                    {"s": "t" * 32, "p": "q" * 63},
                ],
            }
        },
        "n": 2**53 - 1,
        "ack": None,
    }
    cbor_len = len(wirecbor.encode(obj)) + 10  # +10: sig pair (bstr(8) header+tag)
    assert cbor_len <= 640, f"cfg.wifi signed CBOR too large: {cbor_len} bytes"

    json_obj = dict(obj)
    json_len = (
        len(json.dumps(json_obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
        + len(',"sig":""')
        + 11
    )
    assert json_len <= 640, f"cfg.wifi signed JSON too large: {json_len} bytes"


def test_xport_cbor_key_52_round_trips():
    obj = {
        "v": 1,
        "state": "online",
        "mode": "sleep",
        "batt_mv": 3300,
        "rssi": -80,
        "session": "s_aabbccdd",
        "ts": 1_700_000_000,
        "xport": "wifi",
    }
    encoded = wirecbor.encode(obj)
    decoded = wirecbor.decode(encoded)
    assert decoded["xport"] == "wifi"


def test_xport_absent_is_absent_after_cbor_round_trip():
    obj = {
        "v": 1,
        "state": "online",
        "mode": "sleep",
        "batt_mv": 3300,
        "rssi": -80,
        "session": "s_aabbccdd",
        "ts": 1_700_000_000,
    }
    decoded = wirecbor.decode(wirecbor.encode(obj))
    assert "xport" not in decoded


# ---------------------------------------------------------------------------
# app/devcfg.py: push_wifi / wifi_pending / ack / republish
# ---------------------------------------------------------------------------


def test_push_wifi_en_only_omits_nets_key(broker: FakeBrokerClient):
    _make_user("student20", "student20")
    _make_pager_device("pgr-wifi-1", "student20")

    ok = devcfg.push_wifi("pgr-wifi-1", en=True, nets=None, broker=broker)

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg"
    assert sent["cfg"] == {"wifi": {"en": True}}
    assert "nets" not in sent["cfg"]["wifi"]
    assert sent["ack"] is None


def test_push_wifi_with_nets_replaces(broker: FakeBrokerClient):
    _make_user("student21", "student21")
    _make_pager_device("pgr-wifi-2", "student21")

    ok = devcfg.push_wifi(
        "pgr-wifi-2", en=True, nets=[{"s": "home", "p": "hunter22"}], broker=broker
    )

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"] == {"wifi": {"en": True, "nets": [{"s": "home", "p": "hunter22"}]}}

    raw = _raw_device("pgr-wifi-2")
    assert raw["pendingCfgWifi"]["id"] == sent["id"]
    assert raw["pendingCfgWifi"]["acked"] is False


def test_push_wifi_empty_nets_list_clears(broker: FakeBrokerClient):
    _make_user("student22", "student22")
    _make_pager_device("pgr-wifi-3", "student22")

    ok = devcfg.push_wifi("pgr-wifi-3", en=False, nets=[], broker=broker)

    assert ok is True
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"] == {"wifi": {"en": False, "nets": []}}


def test_push_wifi_unregistered_device_returns_false(broker: FakeBrokerClient):
    assert devcfg.push_wifi("no-such-device", en=True, nets=None, broker=broker) is False
    assert broker.published == []


def test_wifi_pending_false_before_any_push_and_after_ack(broker: FakeBrokerClient):
    _make_user("student23", "student23")
    _make_pager_device("pgr-wifi-4", "student23")
    assert devcfg.wifi_pending("pgr-wifi-4") is False

    devcfg.push_wifi(
        "pgr-wifi-4", en=True, nets=[{"s": "home", "p": "hunter22"}], broker=broker
    )
    assert devcfg.wifi_pending("pgr-wifi-4") is True
    sent_id = json.loads(broker.published[0].payload)["id"]

    assert devcfg.ack("pgr-wifi-4", sent_id) is True
    assert devcfg.wifi_pending("pgr-wifi-4") is False


def test_pending_cfg_wifi_independent_of_pending_lock_and_sms(broker: FakeBrokerClient):
    _make_user("student24", "student24")
    _make_pager_device("pgr-wifi-5", "student24")

    devcfg.push_cfg("pgr-wifi-5", {"auto": 5}, broker)
    devcfg.push_wifi(
        "pgr-wifi-5", en=True, nets=[{"s": "home", "p": "hunter22"}], broker=broker
    )
    lock_id = _raw_device("pgr-wifi-5")["pendingCfg"]["id"]
    wifi_id = _raw_device("pgr-wifi-5")["pendingCfgWifi"]["id"]
    assert lock_id != wifi_id
    broker.clear()

    devcfg.republish_pending("pgr-wifi-5", broker)
    assert len(broker.published) == 2
    sent_ids = {json.loads(p.payload)["id"] for p in broker.published}
    assert sent_ids == {lock_id, wifi_id}

    assert devcfg.ack("pgr-wifi-5", wifi_id) is True
    assert _raw_device("pgr-wifi-5")["pendingCfg"]["acked"] is False
    assert devcfg.ack("pgr-wifi-5", lock_id) is True
    assert _raw_device("pgr-wifi-5")["pendingCfgWifi"]["acked"] is True


def test_push_wifi_two_maximal_entries_fits_both_encodings(broker: FakeBrokerClient):
    _make_user("student25", "student25")
    _make_pager_device("pgr-wifi-6", "student25")
    nets = [{"s": "s" * 32, "p": "p" * 63}, {"s": "t" * 32, "p": "q" * 63}]

    ok = devcfg.push_wifi("pgr-wifi-6", en=True, nets=nets, broker=broker)
    assert ok is True  # would have raised AssertionError from
    # `_assert_within_envelope_limit` if either encoding overflowed 640 bytes.


def test_push_wifi_never_logs_a_psk(broker: FakeBrokerClient, caplog: pytest.LogCaptureFixture):
    _make_user("student26", "student26")
    _make_pager_device("pgr-wifi-7", "student26")
    secret_psk = "sekrit-passphrase"

    with caplog.at_level("DEBUG"):
        devcfg.push_wifi(
            "pgr-wifi-7", en=True, nets=[{"s": "home", "p": secret_psk}], broker=broker
        )

    assert all(secret_psk not in rec.message for rec in caplog.records)


# ---------------------------------------------------------------------------
# API: GET/PUT /api/devices/{id}/wifi
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
def client(broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        yield c


@pytest.fixture
def admin_headers() -> dict[str, str]:
    auth_user = fb_auth.create_user(email="wifi-admin@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="wifiadmin", display_name="Admin", role="admin"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"admin": True})
    return auth_header(auth_user.uid)


def test_get_wifi_owner_allowed_defaults(client: TestClient):
    _make_user("wowner1", "wowner1")
    _make_pager_device("pgr-wapi-1", "wowner1")

    resp = client.get("/api/devices/pgr-wapi-1/wifi", headers=auth_header("wowner1"))
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"en": False, "nets": [], "pending": False}


def test_get_wifi_third_party_forbidden(client: TestClient):
    _make_user("wowner2", "wowner2")
    _make_user("wstranger2", "wstranger2")
    _make_pager_device("pgr-wapi-2", "wowner2")

    resp = client.get("/api/devices/pgr-wapi-2/wifi", headers=auth_header("wstranger2"))
    assert resp.status_code == 403


def test_get_wifi_admin_allowed(client: TestClient, admin_headers: dict[str, str]):
    _make_user("wowner3", "wowner3")
    _make_pager_device("pgr-wapi-3", "wowner3")

    resp = client.get("/api/devices/pgr-wapi-3/wifi", headers=admin_headers)
    assert resp.status_code == 200, resp.text


def test_get_wifi_unknown_device_404(client: TestClient):
    _make_user("wowner4", "wowner4")
    resp = client.get("/api/devices/no-such-device/wifi", headers=auth_header("wowner4"))
    assert resp.status_code == 404


def test_put_wifi_en_only_toggles_without_nets(client: TestClient):
    """`en` alone is always allowed, regardless of `tls` -- and never
    disturbs the guard's TLS check (no `nets` field at all in this
    request)."""
    _make_user("wowner5", "wowner5")
    _make_pager_device("pgr-wapi-5", "wowner5")

    resp = client.put(
        "/api/devices/pgr-wapi-5/wifi", json={"en": True}, headers=auth_header("wowner5")
    )
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"en": True, "nets": [], "pending": True}


def test_put_wifi_with_nets_requires_pinned_tls(client: TestClient):
    _make_user("wowner6", "wowner6")
    _make_pager_device("pgr-wapi-6", "wowner6")
    devices_store.update_status("pgr-wapi-6", tls="pinned")

    resp = client.put(
        "/api/devices/pgr-wapi-6/wifi",
        json={"en": True, "nets": [{"s": "home", "p": "hunter22"}]},
        headers=auth_header("wowner6"),
    )
    assert resp.status_code == 200, resp.text
    assert resp.json() == {
        "en": True,
        "nets": [{"s": "home", "set": True}],
        "pending": True,
    }


@pytest.mark.parametrize("tls", [None, "unpinned", "broken"])
def test_put_wifi_with_nets_refused_when_not_pinned(
    client: TestClient, caplog: pytest.LogCaptureFixture, tls: str | None
):
    _make_user("wowner7", "wowner7")
    _make_pager_device("pgr-wapi-7", "wowner7")
    if tls is not None:
        devices_store.update_status("pgr-wapi-7", tls=tls)

    with caplog.at_level("ERROR", logger="relay.devices"):
        resp = client.put(
            "/api/devices/pgr-wapi-7/wifi",
            json={"en": True, "nets": [{"s": "home", "p": "hunter22"}]},
            headers=auth_header("wowner7"),
        )

    assert resp.status_code == 409
    assert resp.json() == {
        "detail": (
            "this device is not reporting a verified TLS connection; push a CA first"
        )
    }
    assert any(
        "SECURITY wifi-nets-refused device=pgr-wapi-7" in rec.message for rec in caplog.records
    )
    # The refused push must never even be attempted, let alone log a PSK.
    assert all("hunter22" not in rec.message for rec in caplog.records)


def test_put_wifi_en_only_still_allowed_when_not_pinned(client: TestClient):
    _make_user("wowner8", "wowner8")
    _make_pager_device("pgr-wapi-8", "wowner8")
    devices_store.update_status("pgr-wapi-8", tls="broken")

    resp = client.put(
        "/api/devices/pgr-wapi-8/wifi", json={"en": False}, headers=auth_header("wowner8")
    )
    assert resp.status_code == 200, resp.text


def test_put_wifi_empty_nets_clears_and_is_allowed_only_when_pinned(client: TestClient):
    _make_user("wowner9", "wowner9")
    _make_pager_device("pgr-wapi-9", "wowner9")
    devices_store.update_status("pgr-wapi-9", tls="pinned")
    client.put(
        "/api/devices/pgr-wapi-9/wifi",
        json={"en": True, "nets": [{"s": "home", "p": "hunter22"}]},
        headers=auth_header("wowner9"),
    )

    resp = client.put(
        "/api/devices/pgr-wapi-9/wifi",
        json={"en": True, "nets": []},
        headers=auth_header("wowner9"),
    )
    assert resp.status_code == 200, resp.text
    assert resp.json() == {"en": True, "nets": [], "pending": True}


def test_get_wifi_never_returns_a_psk(client: TestClient):
    _make_user("wowner10", "wowner10")
    _make_pager_device("pgr-wapi-10", "wowner10")
    devices_store.update_status("pgr-wapi-10", tls="pinned")
    client.put(
        "/api/devices/pgr-wapi-10/wifi",
        json={
            "en": True,
            "nets": [{"s": "home", "p": "hunter22"}, {"s": "office", "p": "correcthorse"}],
        },
        headers=auth_header("wowner10"),
    )

    resp = client.get("/api/devices/pgr-wapi-10/wifi", headers=auth_header("wowner10"))
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["nets"] == [
        {"s": "home", "set": True},
        {"s": "office", "set": True},
    ]
    assert "hunter22" not in resp.text
    assert "correcthorse" not in resp.text
    assert all("p" not in net for net in body["nets"])


def test_put_wifi_over_max_nets_is_422(client: TestClient):
    _make_user("wowner11", "wowner11")
    _make_pager_device("pgr-wapi-11", "wowner11")
    devices_store.update_status("pgr-wapi-11", tls="pinned")

    resp = client.put(
        "/api/devices/pgr-wapi-11/wifi",
        json={
            "en": True,
            "nets": [
                {"s": "one", "p": "password1"},
                {"s": "two", "p": "password2"},
                {"s": "three", "p": "password3"},
            ],
        },
        headers=auth_header("wowner11"),
    )
    assert resp.status_code == 422


def test_put_wifi_bad_ssid_or_psk_length_is_422(client: TestClient):
    _make_user("wowner12", "wowner12")
    _make_pager_device("pgr-wapi-12", "wowner12")
    devices_store.update_status("pgr-wapi-12", tls="pinned")

    too_short_psk = client.put(
        "/api/devices/pgr-wapi-12/wifi",
        json={"en": True, "nets": [{"s": "home", "p": "short"}]},
        headers=auth_header("wowner12"),
    )
    assert too_short_psk.status_code == 422

    empty_ssid = client.put(
        "/api/devices/pgr-wapi-12/wifi",
        json={"en": True, "nets": [{"s": "", "p": "hunter22"}]},
        headers=auth_header("wowner12"),
    )
    assert empty_ssid.status_code == 422


def test_put_wifi_forbidden_for_non_owner(client: TestClient):
    _make_user("wowner13", "wowner13")
    _make_user("wstranger13", "wstranger13")
    _make_pager_device("pgr-wapi-13", "wowner13")

    resp = client.put(
        "/api/devices/pgr-wapi-13/wifi",
        json={"en": True},
        headers=auth_header("wstranger13"),
    )
    assert resp.status_code == 403


def test_put_wifi_pushes_cfg_matching_wificred_parser_shape(
    client: TestClient, broker: FakeBrokerClient
):
    """The pushed `/down cfg.wifi` must decode, over CBOR, to exactly the
    sub-map shape `firmware/main/wificred.c`'s `wificred_parse_cfg_submap`
    expects: `cfg` key 38 -> `wifi` key 3 -> `{en: 0, nets: 1: [{s: 0, p: 1}]}`."""
    _make_user("wowner14", "wowner14")
    _make_pager_device("pgr-wapi-14", "wowner14")
    devices_store.update_status("pgr-wapi-14", tls="pinned")

    resp = client.put(
        "/api/devices/pgr-wapi-14/wifi",
        json={"en": True, "nets": [{"s": "home", "p": "hunter22"}]},
        headers=auth_header("wowner14"),
    )
    assert resp.status_code == 200, resp.text

    sent = json.loads(broker.published[0].payload)
    cbor_bytes = wirecbor.encode(sent)
    int_keyed = wirecbor.translate_to_int(sent)
    assert int_keyed[wirecbor.KEYMAP["cfg"]][wirecbor.CFG_KEYMAP["wifi"]] == {
        wirecbor.WIFI_KEYMAP["en"]: True,
        wirecbor.WIFI_KEYMAP["nets"]: [
            {wirecbor.WIFI_NET_KEYMAP["s"]: "home", wirecbor.WIFI_NET_KEYMAP["p"]: "hunter22"}
        ],
    }
    decoded = wirecbor.decode(cbor_bytes)
    assert decoded["cfg"]["wifi"] == {"en": True, "nets": [{"s": "home", "p": "hunter22"}]}

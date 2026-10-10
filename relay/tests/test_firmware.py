"""docs/OTA_DESIGN.md §5: the firmware index, delta choice, `cfg.ota` shape,
`/status` OTA fields and the `cfg.ota` pending slot. The admin routes are
tested in tests/test_admin.py (same fixtures)."""

from __future__ import annotations

import copy
import json
from collections.abc import Iterator

import cbor2
import httpx
import pytest
from pydantic import ValidationError

from app import devcfg, firmware, wirecbor
from app.config import Settings
from app.ingest import Ingest
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import users as users_store
from app.wire import StatusEnvelope
from tests.conftest import online_status_payload, status_topic
from tests.fake_transport import FakeBrokerClient

BASE_URL = "https://storage.googleapis.com/fwbkt/"
INDEX_URL = BASE_URL + "fw/index.json"

ID_NEW = "a1" * 32  # target build
ID_OLD = "b2" * 32  # a previous release (delta base)
ID_OTHER = "c3" * 32  # a build with no delta for ID_OLD


def _obj(path: str, osz: int, sha: str) -> dict:
    return {"path": path, "osz": osz, "osha": sha}


def make_index() -> dict:
    new16, old16 = ID_NEW[:16], ID_OLD[:16]
    return {
        "v": 1,
        "builds": [
            {
                "id": ID_NEW,
                "version": "beta-57-gdeadbee",
                "size": 685168,
                "published": 1_790_000_000,
                "full": _obj(f"fw/{new16}/full.z", 338784, "d4" * 32),
                "deltas": [
                    {
                        **_obj(f"fw/{new16}/from-{old16}.dz", 42513, "e5" * 32),
                        "base": ID_OLD,
                        "psz": 686466,
                    }
                ],
            },
            {
                "id": ID_OLD,
                "version": "beta-56-g20ecbd5",
                "size": 680000,
                "published": 1_780_000_000,
                "full": _obj(f"fw/{old16}/full.z", 330000, "f6" * 32),
                "deltas": [],
            },
        ],
    }


def settings(**kw: object) -> Settings:
    d: dict = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "k",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
        "fw_index_url": INDEX_URL,
        "fw_bucket_base": BASE_URL,
    }
    d.update(kw)
    return Settings(**d)


@pytest.fixture
def index_server() -> Iterator[dict]:
    """Serves `state["index"]` (a dict, or raw bytes) at INDEX_URL; counts
    fetches in `state["hits"]`."""
    state: dict = {"index": make_index(), "hits": 0, "status": 200}

    def handler(request: httpx.Request) -> httpx.Response:
        assert str(request.url) == INDEX_URL
        state["hits"] += 1
        body = state["index"]
        content = body if isinstance(body, bytes) else json.dumps(body).encode()
        return httpx.Response(state["status"], content=content)

    firmware.reset_cache()
    firmware._transport = httpx.MockTransport(handler)
    yield state
    firmware._transport = None
    firmware.reset_cache()


# ---- index validation ----


def test_index_parses():
    idx = firmware.FirmwareIndex.model_validate(make_index())
    assert [b.id16 for b in idx.builds] == [ID_NEW[:16], ID_OLD[:16]]


@pytest.mark.parametrize(
    "mutate",
    [
        lambda i: i["builds"][0]["full"].update(path="fw/../etc/passwd"),
        lambda i: i["builds"][0]["full"].update(path="fw/abc/full.z"),
        lambda i: i["builds"][0]["deltas"][0].update(path="fw/" + "a1" * 8 + "/from-zz.dz"),
        lambda i: i["builds"][0]["full"].update(osz=0x200001),
        lambda i: i["builds"][0].update(size=0),
        lambda i: i["builds"][0]["deltas"][0].update(psz=0x200001),
        lambda i: i["builds"][0].update(id="A1" * 32),
        lambda i: i["builds"][0].update(id="a1" * 31),
        lambda i: i["builds"][0]["full"].update(osha="xyz"),
        lambda i: i["builds"][0]["deltas"][0].update(base="b2" * 31),
        lambda i: i.update(v=2),
    ],
)
def test_index_rejects_bad_values(mutate):
    bad = copy.deepcopy(make_index())
    mutate(bad)
    with pytest.raises(ValidationError):
        firmware.FirmwareIndex.model_validate(bad)


def test_index_accepts_max_size():
    ok = make_index()
    ok["builds"][0]["full"]["osz"] = 0x200000
    firmware.FirmwareIndex.model_validate(ok)


def test_load_index_caches_for_a_minute(index_server):
    s = settings()
    firmware.load_index(s)
    firmware.load_index(s)
    assert index_server["hits"] == 1


def test_load_index_http_error_and_bad_json_are_unavailable(index_server):
    index_server["status"] = 500
    with pytest.raises(firmware.FirmwareIndexUnavailable):
        firmware.load_index(settings())
    index_server["status"] = 200
    index_server["index"] = b"not json"
    with pytest.raises(firmware.FirmwareIndexUnavailable):
        firmware.load_index(settings())
    bad = make_index()
    bad["builds"][0]["full"]["path"] = "elsewhere"
    index_server["index"] = bad
    with pytest.raises(firmware.FirmwareIndexUnavailable):
        firmware.load_index(settings())


def test_load_index_unconfigured():
    with pytest.raises(firmware.FirmwareNotConfigured):
        firmware.load_index(settings(fw_index_url=None))
    with pytest.raises(firmware.FirmwareNotConfigured):
        firmware.load_index(settings(fw_bucket_base=None))


# ---- choose / build / estimate ----


def test_choose_delta_only_on_exact_base16_match():
    idx = firmware.FirmwareIndex.model_validate(make_index())
    c = firmware.choose(idx, ID_NEW[:16], ID_OLD[:16])
    assert c.kind == "delta" and c.obj.osz == 42513
    # off by one hex char, None, and a device already on some other image
    off = ID_OLD[:15] + ("0" if ID_OLD[15] != "0" else "1")
    assert firmware.choose(idx, ID_NEW[:16], off).kind == "full"
    assert firmware.choose(idx, ID_NEW[:16], None).kind == "full"
    assert firmware.choose(idx, ID_NEW[:16], ID_OTHER[:16]).kind == "full"
    # a target with no deltas is always full
    assert firmware.choose(idx, ID_OLD[:16], ID_OLD[:16]).kind == "full"
    with pytest.raises(KeyError):
        firmware.choose(idx, "0" * 16, None)


def test_choose_never_picks_delta_with_base_equal_target():
    raw = make_index()
    build = raw["builds"][0]
    assert build["id"][:16] == ID_NEW[:16]
    build["deltas"].append({**build["deltas"][0], "base": build["id"]})
    idx = firmware.FirmwareIndex.model_validate(raw)
    c = firmware.choose(idx, ID_NEW[:16], ID_NEW[:16])
    assert c.kind == "full"


def test_estimate_bytes():
    assert firmware.estimate_bytes(338784) == int(338784 * 1.045) + 7168
    assert firmware.estimate_bytes(0) == 7168


def test_cfg_ota_shapes_and_envelope_limit():
    idx = firmware.FirmwareIndex.model_validate(make_index())
    s = settings()
    full = firmware.build_cfg_ota(firmware.choose(idx, ID_NEW[:16], None), s)
    assert full == {
        "img": ID_NEW,
        "isz": 685168,
        "url": BASE_URL + f"fw/{ID_NEW[:16]}/full.z",
        "osz": 338784,
        "osha": "d4" * 32,
        "fmt": "full",
    }
    delta = firmware.build_cfg_ota(firmware.choose(idx, ID_NEW[:16], ID_OLD[:16]), s)
    assert delta["fmt"] == "delta"
    assert delta["base"] == ID_OLD and delta["psz"] == 686466
    assert delta["url"].endswith(f"from-{ID_OLD[:16]}.dz")
    # worst case: a 90-char URL, still inside the 640 B signed-JSON limit
    long_base = "https://storage.googleapis.com/" + "x" * 14 + "/"
    long_delta = firmware.build_cfg_ota(
        firmware.choose(idx, ID_NEW[:16], ID_OLD[:16]), settings(fw_bucket_base=long_base)
    )
    assert len(long_delta["url"]) == 90
    # D12: the `fw-cache/` path is 6 chars longer than `fw/`... (96 chars)
    cache_delta = firmware.FwDelta(
        path=f"fw-cache/{ID_NEW[:16]}/from-{ID_OLD[:16]}.dz", osz=42513, osha="e5" * 32,
        base=ID_OLD, psz=686466,
    )
    long_cache = firmware.build_cfg_ota(
        firmware.Choice("delta", idx.find(ID_NEW[:16]), cache_delta, on_demand=True),
        settings(fw_bucket_base=long_base),
    )
    assert len(long_cache["url"]) == 96
    for cfg in (full, delta, long_delta, long_cache):
        devcfg._assert_within_envelope_limit(
            {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "kind": "cfg",
             "cfg": {"ota": cfg}, "ack": None}
        )


# ---- status ingest ----


def _device(device_id: str = "pgr-ota-1") -> None:
    users_store.create_user(uid="u-ota", alias="uota", display_name="uota")
    devices_store.create_device(
        device_id=device_id,
        owner_uid="u-ota",
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )
    backends_store.create_backend("u-ota", kind="pager", config={"deviceId": device_id}, enabled=True)


def test_status_envelope_ota_fields_valid():
    env = StatusEnvelope.model_validate(
        {
            "state": "online", "mode": "sleep", "batt_mv": 3300, "ts": 1_700_000_000,
            "session": "s_00000001", "img": "0123456789abcdef", "ota": 1,
            "ota_t": "fedcba9876543210", "ota_st": "dl", "ota_pct": 50, "ota_err": "http",
        }
    )
    assert (env.img, env.ota, env.ota_t, env.ota_st, env.ota_pct, env.ota_err) == (
        "0123456789abcdef", 1, "fedcba9876543210", "dl", 50, "http",
    )


def test_status_envelope_bad_ota_fields_drop_only_that_field():
    env = StatusEnvelope.model_validate(
        {
            "state": "online", "mode": "sleep", "batt_mv": 3300, "ts": 1_700_000_000,
            "session": "s_00000001", "img": "0123456789ABCDEF", "ota": 2,
            "ota_t": "abc", "ota_st": "bogus", "ota_pct": 101, "ota_err": "TOOLONGERR1",
            "fw": "ok-fw",
        }
    )
    assert env.img is None and env.ota is None and env.ota_t is None
    assert env.ota_st is None and env.ota_pct is None and env.ota_err is None
    assert env.fw == "ok-fw"
    for bad in ({"ota_pct": -1}, {"ota": True}, {"ota_err": "a1"}, {"ota_st": 5}):
        e = StatusEnvelope.model_validate(
            {"state": "offline", "session": "s_00000001", **bad}
        )
        assert getattr(e, next(iter(bad))) is None


def test_ingest_stores_ota_status_fields_and_drops_bad_ones():
    _device()
    ingest = Ingest(FakeBrokerClient(), None)  # type: ignore[arg-type]
    ingest.handle_status(
        status_topic("pgr-ota-1"),
        online_status_payload(
            "s_00000001", img="0123456789abcdef", ota=1, ota_t="fedcba9876543210",
            ota_st="dl", ota_pct=40, ota_err="zzzz",
        ),
    )
    st = devices_store.get_device("pgr-ota-1").status
    assert (st.img, st.otaCap, st.otaTarget, st.otaState, st.otaPct, st.otaErr) == (
        "0123456789abcdef", 1, "fedcba9876543210", "dl", 40, "zzzz",
    )
    ingest.handle_status(
        status_topic("pgr-ota-1"),
        online_status_payload("s_00000002", img="nothex", ota_st="dl", ota_pct=500),
    )
    st = devices_store.get_device("pgr-ota-1").status
    assert st.otaState == "dl"
    assert st.otaPct == 40  # bad value dropped, previous kept
    assert st.img == "0123456789abcdef"


def test_ingest_clears_otajob_when_job_finishes():
    _device()
    broker = FakeBrokerClient()
    ingest = Ingest(broker, None)  # type: ignore[arg-type]
    job = {"target16": "fedcba9876543210", "kind": "full", "osz": 1, "estBytes": 2, "by_uid": "x"}
    devcfg.push_ota("pgr-ota-1", {"img": "ab" * 32}, broker, job=job)
    assert devices_store.get_device("pgr-ota-1").otaJob.target16 == "fedcba9876543210"
    ingest.handle_status(
        status_topic("pgr-ota-1"),
        online_status_payload("s_00000001", ota_t="fedcba9876543210", ota_st="dl", ota_pct=10),
    )
    assert devices_store.get_device("pgr-ota-1").otaJob is not None
    ingest.handle_status(
        status_topic("pgr-ota-1"),
        online_status_payload("s_00000001", ota_t="fedcba9876543210", ota_st="fail", ota_err="hash"),
    )
    assert devices_store.get_device("pgr-ota-1").otaJob is None


# ---- devcfg ----


def test_push_ota_stores_pending_job_and_republishes_until_acked():
    _device()
    broker = FakeBrokerClient()
    job = {"target16": ID_NEW[:16], "kind": "delta", "osz": 42513, "estBytes": 51000, "by_uid": "su"}
    cfg = {"img": ID_NEW, "fmt": "full"}
    assert devcfg.push_ota("pgr-ota-1", cfg, broker, job=job) is True
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "cfg" and sent["cfg"]["ota"]["fmt"] == "full"
    dev = devices_store.get_device("pgr-ota-1")
    assert dev.otaJob.target16 == ID_NEW[:16] and dev.otaJob.by_uid == "su"
    assert dev.otaJob.at is not None
    snap = devcfg._devices().document("pgr-ota-1").get().to_dict()
    assert snap["pendingCfgOta"]["id"] == sent["id"] and snap["pendingCfgOta"]["acked"] is False

    broker.clear()
    devcfg.republish_pending("pgr-ota-1", broker)
    assert [json.loads(p.payload)["id"] for p in broker.published] == [sent["id"]]

    assert devcfg.ack("pgr-ota-1", sent["id"]) is True
    broker.clear()
    devcfg.republish_pending("pgr-ota-1", broker)
    assert broker.published == []


def test_cancel_ota_pushes_cancel_and_clears_job():
    _device()
    broker = FakeBrokerClient()
    job = {"target16": ID_NEW[:16], "kind": "full", "osz": 1, "estBytes": 2, "by_uid": "su"}
    devcfg.push_ota("pgr-ota-1", {"img": ID_NEW}, broker, job=job)
    broker.clear()
    assert devcfg.cancel_ota("pgr-ota-1", broker) is True
    assert json.loads(broker.published[0].payload)["cfg"] == {"ota": {"cancel": True}}
    assert devices_store.get_device("pgr-ota-1").otaJob is None


def test_push_ota_unknown_device_returns_false():
    assert devcfg.push_ota("nope", {"img": ID_NEW}, FakeBrokerClient()) is False
    assert devcfg.cancel_ota("nope", FakeBrokerClient()) is False


# ---- wire encoding (CBOR) ----


def _round_trip_cfg(cfg_ota: dict) -> tuple[dict, bytes]:
    obj = {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "kind": "cfg",
           "cfg": {"ota": cfg_ota}, "ack": None}
    raw = wirecbor.encode(obj)
    return wirecbor.decode(raw), raw


def test_cbor_round_trip_full_and_delta_and_cancel():
    idx = firmware.FirmwareIndex.model_validate(make_index())
    for img in (None, ID_OLD[:16]):
        cfg = firmware.build_cfg_ota(firmware.choose(idx, ID_NEW[:16], img), settings())
        back, raw = _round_trip_cfg(cfg)
        assert back["cfg"]["ota"] == cfg
        # hashes are 32-byte bstr on the wire, fmt is an int
        wire_cfg = cbor2.loads(raw)[wirecbor.KEYMAP["cfg"]][4]
        assert wire_cfg[0] == bytes.fromhex(ID_NEW) and wire_cfg[4] == bytes.fromhex(cfg["osha"])
        assert wire_cfg[5] == (1 if img else 0)
        assert (6 in wire_cfg) == bool(img)
    back, _ = _round_trip_cfg({"cancel": True})
    assert back["cfg"]["ota"] == {"cancel": True}

# ruff: noqa: F811
"""docs/ASSETS_OTA_DESIGN.md §3: the font (assets) OTA on the relay side --
wire keys 72/73 and `cfg.ast`, the assets index, `pendingCfgAst`, the
`/assets` routes and their 409 rules."""

from __future__ import annotations

import copy
import json
from collections.abc import Iterator

import cbor2
import httpx
import pytest
from fastapi.testclient import TestClient
from pydantic import ValidationError

from app import devcfg, firmware, wirecbor
from app.ingest import Ingest
from app.main import create_app
from app.store import devices as devices_store
from app.wire import StatusEnvelope
from tests.conftest import online_status_payload, status_topic
from tests.fake_transport import FakeBrokerClient
from tests.test_admin import (  # noqa: F401 -- fixtures
    FakeEmqxAdmin,
    admin_headers,
    fake_emqx,
    make_settings,
)
from tests.test_family_router import (
    _make_family,
    _make_family_admin,
    _make_member,
    _make_pager_device,
)
from tests.test_firmware import BASE_URL, ID_NEW, make_index

AS_INDEX_URL = BASE_URL + "as/index.json"
AS_ID = "5a" * 32
AS_ID2 = "6b" * 32
ID16 = AS_ID[:16]


def make_as_index() -> dict:
    def b(i: str, label: str, pub: int, size: int, osz: int) -> dict:
        return {
            "id": i, "label": label, "lang": "en", "fv": 1, "size": size, "published": pub,
            "full": {"path": f"as/{i[:16]}/full.z", "osz": osz, "osha": "d7" * 32},
        }

    return {
        "v": 1,
        "builds": [
            b(AS_ID, "dejavu+emoji", 1_790_000_000, 736237, 379008),
            b(AS_ID2, "dejavu", 1_780_000_000, 500000, 250000),
        ],
        "withdrawn": [],
    }


@pytest.fixture
def as_server() -> Iterator[dict]:
    state: dict = {"index": make_as_index(), "status": 200}

    def handler(request: httpx.Request) -> httpx.Response:
        assert str(request.url) in (AS_INDEX_URL, BASE_URL + "fw/index.json")
        if str(request.url) != AS_INDEX_URL:
            return httpx.Response(200, content=json.dumps(make_index()).encode())
        body = state["index"]
        content = body if isinstance(body, bytes) else json.dumps(body).encode()
        return httpx.Response(state["status"], content=content)

    firmware.reset_cache()
    firmware._transport = httpx.MockTransport(handler)
    yield state
    firmware._transport = None
    firmware.reset_cache()


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def ac(
    fake_emqx: FakeEmqxAdmin, broker: FakeBrokerClient, as_server: dict
) -> Iterator[TestClient]:
    app = create_app(
        settings=make_settings(fw_index_url=BASE_URL + "fw/index.json", fw_bucket_base=BASE_URL),
        broker_client=broker,
    )
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        yield c


def _dev(device_id: str = "pgr-as-1", *, ast: int | None = 1, ast_id: str | None = None) -> None:
    devices_store.create_device(
        device_id=device_id, owner_uid="owner-as", label="d", mqtt_username=device_id,
        mqtt_password_hash="x", auth_mode="password",
    )
    devices_store.update_status(device_id, state="online", otaCap=1, assetsSrc=ast, assetsId=ast_id)


# ---- wire: keys 72/73, cfg.ast, fv ----


def test_cbor_status_keys_72_73_round_trip():
    raw = cbor2.dumps({1: 1, 72: 2, 73: "0123456789abcdef"})
    # key 1 is not asserted on; only the new keys matter here
    names = wirecbor.translate_to_names(cbor2.loads(raw))
    assert names["ast"] == 2 and names["ast_id"] == "0123456789abcdef"
    assert wirecbor.translate_to_int({"ast": 1})[72] == 1


def test_cfg_ast_round_trip_with_fv():
    cfg = {
        "img": AS_ID, "isz": 736237, "url": BASE_URL + f"as/{ID16}/full.z",
        "osz": 379008, "osha": "d7" * 32, "fmt": "full", "fv": 1,
    }
    obj = {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "kind": "cfg",
           "cfg": {"ast": cfg}, "ack": None}
    raw = wirecbor.encode(obj)
    assert wirecbor.decode(raw)["cfg"]["ast"] == cfg
    wire_cfg = cbor2.loads(raw)[wirecbor.KEYMAP["cfg"]][6]
    assert wire_cfg[9] == 1 and wire_cfg[0] == bytes.fromhex(AS_ID) and wire_cfg[5] == 0
    back = wirecbor.decode(wirecbor.encode({**obj, "cfg": {"ast": {"cancel": True}}}))
    assert back["cfg"]["ast"] == {"cancel": True}


def test_status_envelope_ast_fields():
    base = {"state": "online", "mode": "sleep", "batt_mv": 3300, "ts": 1_700_000_000,
            "session": "s_00000001"}
    e = StatusEnvelope.model_validate({**base, "ast": 2, "ast_id": "0123456789abcdef"})
    assert (e.ast, e.ast_id) == (2, "0123456789abcdef")
    for bad in ({"ast": 3}, {"ast": -1}, {"ast": True}, {"ast_id": "0123456789ABCDEF"},
                {"ast_id": "abc"}, {"ast_id": 5}):
        e = StatusEnvelope.model_validate({**base, **bad})
        assert getattr(e, next(iter(bad))) is None
    e = StatusEnvelope.model_validate(base)
    assert e.ast is None and e.ast_id is None


# ---- ingest ----


def test_ingest_stores_and_clears_assets_status():
    _dev(ast=None)
    ingest = Ingest(FakeBrokerClient(), None)  # type: ignore[arg-type]
    ingest.handle_status(
        status_topic("pgr-as-1"),
        online_status_payload("s_00000001", ast=2, ast_id="0123456789abcdef"),
    )
    st = devices_store.get_device("pgr-as-1").status
    assert (st.assetsSrc, st.assetsId) == (2, "0123456789abcdef")
    ingest.handle_status(status_topic("pgr-as-1"), online_status_payload("s_00000002", ast=1))
    st = devices_store.get_device("pgr-as-1").status
    assert (st.assetsSrc, st.assetsId) == (1, None)
    ingest.handle_status(status_topic("pgr-as-1"), online_status_payload("s_00000003"))
    st = devices_store.get_device("pgr-as-1").status
    assert (st.assetsSrc, st.assetsId) == (None, None)


def _job(target16: str = ID16) -> dict:
    return {"target16": target16, "kind": "full", "osz": 379008, "estBytes": 1, "by_uid": "x"}


def test_assets_job_finishes_on_terminal_state_or_active_id():
    _dev()
    broker = FakeBrokerClient()
    ingest = Ingest(broker, None)  # type: ignore[arg-type]
    devcfg.push_ast("pgr-as-1", {"img": AS_ID}, broker, job=_job())
    assert devices_store.get_device("pgr-as-1").otaJob.part == "assets"
    ingest.handle_status(
        status_topic("pgr-as-1"),
        online_status_payload("s_00000001", ast=1, ota_t=ID16, ota_st="dl", ota_pct=10),
    )
    assert devices_store.get_device("pgr-as-1").otaJob is not None
    ingest.handle_status(
        status_topic("pgr-as-1"),
        online_status_payload("s_00000001", ast=1, ota_t=ID16, ota_st="fail", ota_err="hash"),
    )
    assert devices_store.get_device("pgr-as-1").otaJob is None
    # active id equal to the target also finishes it (power loss before `ok`)
    devcfg.push_ast("pgr-as-1", {"img": AS_ID}, broker, job=_job())
    ingest.handle_status(
        status_topic("pgr-as-1"), online_status_payload("s_00000002", ast=2, ast_id=ID16)
    )
    assert devices_store.get_device("pgr-as-1").otaJob is None


def test_app_job_is_not_finished_by_ast_id():
    _dev()
    broker = FakeBrokerClient()
    ingest = Ingest(broker, None)  # type: ignore[arg-type]
    devcfg.push_ota("pgr-as-1", {"img": AS_ID}, broker, job=_job())
    ingest.handle_status(
        status_topic("pgr-as-1"), online_status_payload("s_00000001", ast=2, ast_id=ID16)
    )
    assert devices_store.get_device("pgr-as-1").otaJob.part == "app"


def test_online_status_without_ast_drops_pending_and_assets_job():
    _dev()
    broker = FakeBrokerClient()
    ingest = Ingest(broker, None)  # type: ignore[arg-type]
    devcfg.push_ast("pgr-as-1", {"img": AS_ID}, broker, job=_job())
    snap = devcfg._devices().document("pgr-as-1").get().to_dict()
    assert snap["pendingCfgAst"]["acked"] is False
    # with ast the slot survives and is republished on the online edge
    ingest.handle_status(status_topic("pgr-as-1"), online_status_payload("s_00000001", ast=1))
    assert "pendingCfgAst" in devcfg._devices().document("pgr-as-1").get().to_dict()
    broker.clear()
    ingest.handle_status(status_topic("pgr-as-1"), online_status_payload("s_00000002"))
    snap = devcfg._devices().document("pgr-as-1").get().to_dict()
    assert "pendingCfgAst" not in snap
    assert devices_store.get_device("pgr-as-1").otaJob is None
    assert not any(b"ast" in p.payload for p in broker.published)


def test_pending_ast_republished_until_acked_and_cancel():
    _dev()
    broker = FakeBrokerClient()
    devcfg.push_ast("pgr-as-1", {"img": AS_ID}, broker, job=_job())
    sent = json.loads(broker.published[0].payload)
    assert sent["cfg"] == {"ast": {"img": AS_ID}}
    broker.clear()
    devcfg.republish_pending("pgr-as-1", broker)
    assert [json.loads(p.payload)["id"] for p in broker.published] == [sent["id"]]
    assert devcfg.ack("pgr-as-1", sent["id"]) is True
    broker.clear()
    assert devcfg.cancel_ast("pgr-as-1", broker) is True
    assert json.loads(broker.published[0].payload)["cfg"] == {"ast": {"cancel": True}}
    assert devices_store.get_device("pgr-as-1").otaJob is None
    assert devcfg.push_ast("nope", {}, broker) is False and devcfg.cancel_ast("nope", broker) is False


# ---- firmware.py: index + cfg ----


def test_assets_index_validation():
    firmware.AssetsIndex.model_validate(make_as_index())
    ok = make_as_index()
    ok["builds"][0]["size"] = 0x200000 - 4096
    firmware.AssetsIndex.model_validate(ok)
    for mutate in (
        lambda i: i["builds"][0].update(size=0x200000 - 4095),
        lambda i: i["builds"][0].update(size=0),
        lambda i: i["builds"][0].update(fv=0),
        lambda i: i["builds"][0].update(fv=256),
        lambda i: i["builds"][0]["full"].update(path="as/../etc/passwd"),
        lambda i: i["builds"][0]["full"].update(path="as/abc/full.z"),
        lambda i: i["builds"][0]["full"].update(path=f"fw/{ID16}/full.z"),
        lambda i: i["builds"][0]["full"].update(path=f"as/{AS_ID2[:16]}/full.z"),
        lambda i: i["builds"][0]["full"].update(osz=0x200001),
        lambda i: i["builds"][0].update(id="5A" * 32),
        lambda i: i.update(v=2),
    ):
        bad = copy.deepcopy(make_as_index())
        mutate(bad)
        with pytest.raises(ValidationError):
            firmware.AssetsIndex.model_validate(bad)


def test_load_assets_index_errors_and_cache(as_server):
    s = make_settings(fw_index_url=BASE_URL + "fw/index.json", fw_bucket_base=BASE_URL)
    assert len(firmware.load_assets_index(s).builds) == 2
    firmware.reset_cache()
    as_server["status"] = 404
    with pytest.raises(firmware.FirmwareIndexUnavailable):
        firmware.load_assets_index(s)
    as_server["status"] = 200
    as_server["index"] = b"nope"
    with pytest.raises(firmware.FirmwareIndexUnavailable):
        firmware.load_assets_index(s)
    with pytest.raises(firmware.FirmwareNotConfigured):
        firmware.load_assets_index(make_settings(fw_bucket_base=None))


def test_cfg_ast_fits_signed_envelope_limit():
    idx = firmware.AssetsIndex.model_validate(make_as_index())
    long_base = "https://storage.googleapis.com/" + "x" * 14 + "/"
    s = make_settings(fw_bucket_base=long_base)
    cfg = firmware.build_cfg_ast(idx.builds[0], s)
    assert cfg == {
        "img": AS_ID, "isz": 736237, "url": long_base + f"as/{ID16}/full.z",
        "osz": 379008, "osha": "d7" * 32, "fmt": "full", "fv": 1,
    }
    obj = {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "kind": "cfg",
           "cfg": {"ast": cfg}, "ack": None}
    devcfg._assert_within_envelope_limit(obj)  # JSON, signed, worst-case n
    assert len(wirecbor.encode(obj)) + 10 < 640


# ---- routes ----


def test_list_assets_marks_active_and_reports_block(ac, admin_headers):
    _dev(ast=2, ast_id=ID16)
    r = ac.get("/api/admin/assets?device=pgr-as-1", headers=admin_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["blocked"] is None
    assert [b["id16"] for b in body["builds"]] == [ID16, AS_ID2[:16]]
    assert body["builds"][0] == {
        "id": AS_ID, "id16": ID16, "full": {"path": f"as/{ID16}/full.z", "osz": 379008, "osha": "d7" * 32}, "label": "dejavu+emoji", "lang": "en", "fv": 1, "size": 736237,
        "published": 1_790_000_000, "osz": 379008, "estBytes": int(379008 * 1.045) + 7168,
        "active": True,
    }
    assert body["builds"][1]["active"] is False
    plain = ac.get("/api/admin/assets", headers=admin_headers).json()
    assert plain["blocked"] is None and not any(b["active"] for b in plain["builds"])
    assert ac.get("/api/admin/assets?device=nope", headers=admin_headers).status_code == 404
    assert ac.get("/api/admin/assets").status_code in (401, 403)


def test_list_assets_blocked_reasons(ac, admin_headers, broker):
    _dev(ast=None)
    r = ac.get("/api/admin/assets?device=pgr-as-1", headers=admin_headers).json()
    assert r["blocked"] == "needs firmware with font OTA and the new partition table"
    devices_store.update_status("pgr-as-1", assetsSrc=0)
    r = ac.get("/api/admin/assets?device=pgr-as-1", headers=admin_headers).json()
    assert r["blocked"].startswith("needs firmware")


def test_assets_routes_502_503(ac, as_server, admin_headers, client_unconfigured=None):
    _dev()
    as_server["status"] = 500
    assert ac.get("/api/admin/assets", headers=admin_headers).status_code == 502
    r = ac.post("/api/admin/devices/pgr-as-1/assets", json={"target": ID16}, headers=admin_headers)
    assert r.status_code == 502


def test_assets_route_503_not_configured(fake_emqx, broker, admin_headers):
    app = create_app(settings=make_settings(), broker_client=broker)
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        _dev()
        assert c.get("/api/admin/assets", headers=admin_headers).status_code == 503
        r = c.post("/api/admin/devices/pgr-as-1/assets", json={"cancel": True}, headers=admin_headers)
        assert r.status_code == 503


def test_push_assets_success_and_cancel(ac, admin_headers, broker):
    _dev(ast=1)
    r = ac.post("/api/admin/devices/pgr-as-1/assets", json={"target": ID16}, headers=admin_headers)
    assert r.status_code == 200, r.text
    assert r.json() == {
        "ok": True, "kind": "full", "osz": 379008, "estBytes": int(379008 * 1.045) + 7168,
    }
    sent = json.loads(broker.published[-1].payload)
    assert sent["cfg"]["ast"] == {
        "img": AS_ID, "isz": 736237, "url": BASE_URL + f"as/{ID16}/full.z",
        "osz": 379008, "osha": "d7" * 32, "fmt": "full", "fv": 1,
    }
    job = devices_store.get_device("pgr-as-1").otaJob
    assert (job.part, job.target16, job.kind, job.osz) == ("assets", ID16, "full", 379008)
    assert job.by_uid and job.at is not None

    r = ac.post("/api/admin/devices/pgr-as-1/assets", json={"cancel": True}, headers=admin_headers)
    assert r.status_code == 200 and r.json() == {"ok": True}
    assert json.loads(broker.published[-1].payload)["cfg"] == {"ast": {"cancel": True}}
    assert devices_store.get_device("pgr-as-1").otaJob is None


def test_push_assets_409s(ac, admin_headers, broker):
    _dev(ast=None)
    url = "/api/admin/devices/pgr-as-1/assets"
    r = ac.post(url, json={"target": ID16}, headers=admin_headers)
    assert r.status_code == 409
    assert r.json()["detail"] == "needs firmware with font OTA and the new partition table"
    devices_store.update_status("pgr-as-1", assetsSrc=0)
    assert ac.post(url, json={"target": ID16}, headers=admin_headers).status_code == 409
    assert broker.published == []

    devices_store.update_status("pgr-as-1", assetsSrc=2, assetsId=ID16)
    r = ac.post(url, json={"target": ID16}, headers=admin_headers)
    assert r.status_code == 409 and "already" in r.json()["detail"]

    # live job of either kind blocks a push
    devices_store.update_status("pgr-as-1", assetsSrc=1)
    devcfg.push_ota("pgr-as-1", {"img": ID_NEW}, broker, job=_job())
    broker.clear()
    r = ac.post(url, json={"target": ID16}, headers=admin_headers)
    assert r.status_code == 409 and r.json()["detail"] == "another update is in progress"
    # cancelling an app job through /assets must not clear its record
    r = ac.post(url, json={"cancel": True}, headers=admin_headers)
    assert r.status_code == 409
    assert devices_store.get_device("pgr-as-1").otaJob is not None
    assert broker.published == []


def test_ota_push_409_while_assets_job_live(ac, admin_headers, broker):
    _dev(ast=1)
    devcfg.push_ast("pgr-as-1", {"img": AS_ID}, broker, job=_job())
    broker.clear()
    for body in ({"target": ID_NEW[:16]}, {"cancel": True}):
        r = ac.post("/api/admin/devices/pgr-as-1/ota", json=body, headers=admin_headers)
        assert r.status_code == 409, body
        assert r.json()["detail"] == "another update is in progress"
    assert broker.published == []
    assert devices_store.get_device("pgr-as-1").otaJob.part == "assets"


def test_push_assets_404_422(ac, admin_headers):
    _dev()
    url = "/api/admin/devices/pgr-as-1/assets"
    assert ac.post(url, json={"target": "0" * 16}, headers=admin_headers).status_code == 404
    assert ac.post("/api/admin/devices/nope/assets", json={"target": ID16},
                   headers=admin_headers).status_code == 404
    for body in ({}, {"target": "xyz"}, {"target": ID16, "cancel": True}, {"cancel": False}):
        assert ac.post(url, json=body, headers=admin_headers).status_code == 422, body


def test_family_assets_routes(ac, broker):
    fam, other = _make_family("AsA"), _make_family("AsB")
    admin = _make_family_admin("as-adm", "as-adm", fam.id)
    member = _make_member("as-mem", "as-mem", fam.id)
    for did, fid in (("pgr-as-in", fam.id), ("pgr-as-out", other.id)):
        _make_pager_device(did, "owner-as", fid)
        devices_store.update_status(did, state="online", otaCap=1, assetsSrc=2, assetsId=AS_ID2[:16])
    r = ac.get("/api/family/assets?device=pgr-as-in", headers=admin)
    assert r.status_code == 200, r.text
    assert [b["active"] for b in r.json()["builds"]] == [False, True]
    assert ac.get("/api/family/assets?device=pgr-as-out", headers=admin).status_code == 403
    assert ac.get("/api/family/assets", headers=admin).status_code == 422

    body = {"target": ID16}
    r = ac.post("/api/family/devices/pgr-as-in/assets", json=body, headers=admin)
    assert r.status_code == 200, r.text
    assert devices_store.get_device("pgr-as-in").otaJob.part == "assets"
    r = ac.post("/api/family/devices/pgr-as-in/assets", json=body, headers=admin)
    assert r.status_code == 409
    r = ac.post("/api/family/devices/pgr-as-in/assets", json={"cancel": True}, headers=admin)
    assert r.status_code == 200 and r.json() == {"ok": True}
    assert ac.post("/api/family/devices/pgr-as-out/assets", json=body, headers=admin).status_code == 403
    assert ac.post("/api/family/devices/pgr-as-in/assets", json=body, headers=member).status_code == 403


# ---- bs shake counters (keys 12-14) ----


def test_bs_shake_counters_keymap_and_sample():
    from app.store import battery as battery_store

    raw = {0: 1, 1: 60, 2: 0, 3: [0] * 6, 4: 0, 5: 0, 6: 0, 7: [0] * 3, 9: 0, 10: [0] * 3,
           11: 0, 12: 312, 13: 2324, 14: 33}
    names = wirecbor.translate_to_names({wirecbor.KEYMAP["bs"]: raw})["bs"]
    assert (names["sa"], names["sr"], names["sp"]) == (312, 2324, 33)
    bs = battery_store.parse_bs(names)
    assert bs is not None and (bs.sa, bs.sr, bs.sp) == (312, 2324, 33)
    old = battery_store.parse_bs({k: v for k, v in names.items() if k not in ("sa", "sr", "sp")})
    assert old is not None and old.sa is None
    bad = battery_store.parse_bs({**names, "sa": -1})
    assert bad is None

"""Soracom Beam enrolment: app/soracom.py and /api/admin/soracom/* (docs/SORACOM_DESIGN.md §8)."""

from __future__ import annotations

import json
import logging
from collections.abc import Iterator

import httpx
import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import soracom
from app.main import create_app
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header
from tests.test_admin import FakeEmqxAdmin, admin_headers, make_settings  # noqa: F401

KEY_ID = "keyId-SECRETID"
KEY = "secret-SECRETKEY"
GID = "grp-1"


class FakeSoracom:
    def __init__(self, sims: list[dict], groups: list[dict] | None = None, fail: int | None = None):
        self.sims = sims
        self.groups = groups if groups is not None else []
        self.fail = fail
        self.calls: list[tuple[str, str]] = []
        self.set_group: dict[str, str] = {}

    def __call__(self, req: httpx.Request) -> httpx.Response:
        path = req.url.path.removeprefix("/v1")
        self.calls.append((req.method, path))
        if path == "/auth":
            if self.fail == 401:
                return httpx.Response(401, text=f"bad {KEY}")
            return httpx.Response(200, json={"apiKey": "AK", "token": "TK"})
        assert req.headers["x-soracom-api-key"] == "AK"
        assert req.headers["x-soracom-token"] == "TK"
        if self.fail:
            return httpx.Response(self.fail, text="boom body")
        if (req.method, path) == ("GET", "/groups"):
            return httpx.Response(200, json=self.groups)
        if (req.method, path) == ("POST", "/groups"):
            self.groups = [{"groupId": GID, "tags": {"name": "pager-beam"}}]
            return httpx.Response(201, json={"groupId": GID})
        if path.startswith("/groups/") and req.method == "PUT":
            return httpx.Response(200, json={})
        if path == "/subscribers" and req.method == "GET":
            start = int(req.url.params.get("last_evaluated_key", "0"))
            page = self.sims[start : start + 2]
            hdr = {"x-soracom-next-key": str(start + 2)} if start + 2 < len(self.sims) else {}
            return httpx.Response(200, json=page, headers=hdr)
        if path.endswith("/set_group"):
            imsi = path.split("/")[2]
            self.set_group[imsi] = json.loads(req.content)["groupId"]
            return httpx.Response(200, json={})
        raise AssertionError(f"unexpected {req.method} {path}")


def _sim(imsi: str, group: str | None = None, **kw: object) -> dict:
    d: dict = {"imsi": imsi, "iccid": "89" + imsi, "status": "active", "msisdn": "+19995550100"}
    if group:
        d["groupId"] = group
    d.update(kw)
    return d


SIMS = [
    _sim("295050000000001", GID, tags={"name": "one"}, subscription="plan-D"),
    _sim("295050000000002", "other"),
    _sim("295050000000003"),
]


@pytest.fixture(autouse=True)
def _reset_transport() -> Iterator[None]:
    yield
    soracom._transport = None


def _client(configured: bool = True) -> TestClient:
    kw = {"soracom_auth_key_id": KEY_ID, "soracom_auth_key": KEY} if configured else {}
    app = create_app(settings=make_settings(**kw), broker_client=FakeBrokerClient())
    app.state.emqx_admin = FakeEmqxAdmin()
    return TestClient(app)


def _use(fake: FakeSoracom) -> None:
    soracom._transport = httpx.MockTransport(fake)


def test_unconfigured_list(admin_headers):  # noqa: F811
    with _client(False) as c:
        r = c.get("/api/admin/soracom/sims", headers=admin_headers)
    assert r.status_code == 200
    assert r.json() == {"configured": False, "group": "pager-beam", "groupId": None, "sims": []}


def test_unconfigured_enrol_503(admin_headers):  # noqa: F811
    with _client(False) as c:
        assert c.post("/api/admin/soracom/sims/295050000000001/enrol", headers=admin_headers).status_code == 503
        assert c.post("/api/admin/soracom/enrol-all", headers=admin_headers).status_code == 503


def test_list_pages_marks_enrolled_no_msisdn(admin_headers):  # noqa: F811
    fake = FakeSoracom(SIMS, groups=[{"groupId": GID, "tags": {"name": "pager-beam"}}])
    _use(fake)
    with _client() as c:
        r = c.get("/api/admin/soracom/sims", headers=admin_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["configured"] is True and body["groupId"] == GID
    assert [(s["imsi"], s["enrolled"]) for s in body["sims"]] == [
        ("295050000000001", True),
        ("295050000000002", False),
        ("295050000000003", False),
    ]
    assert body["sims"][0]["name"] == "one"
    assert "msisdn" not in r.text and "9995550100" not in r.text
    assert KEY not in r.text


def test_enrol_creates_group_in_tool_order(admin_headers, caplog):  # noqa: F811
    fake = FakeSoracom(SIMS)
    _use(fake)
    caplog.set_level(logging.INFO)
    with _client() as c:
        r = c.post("/api/admin/soracom/sims/295050000000003/enrol", headers=admin_headers)
    assert r.status_code == 200, r.text
    assert r.json() == {"ok": True, "imsi": "295050000000003", "groupId": GID}
    assert fake.calls == [
        ("POST", "/auth"),
        ("GET", "/groups"),
        ("POST", "/groups"),
        ("PUT", f"/groups/{GID}/configuration/SoracomBeam"),
        ("POST", "/subscribers/295050000000003/set_group"),
    ]
    assert fake.set_group == {"295050000000003": GID}
    assert "soracom enrol imsi=...0003 group=grp-1 by=" in caplog.text
    ours = [r.getMessage() for r in caplog.records if r.name == "app.routers.admin"]
    assert len(ours) == 1 and "295050000000003" not in ours[0]


def test_enrol_reuses_group(admin_headers):  # noqa: F811
    fake = FakeSoracom(SIMS, groups=[{"groupId": GID, "tags": {"name": "pager-beam"}}])
    _use(fake)
    with _client() as c:
        r = c.post("/api/admin/soracom/sims/295050000000002/enrol", headers=admin_headers)
    assert r.status_code == 200
    assert ("POST", "/groups") not in fake.calls
    assert fake.set_group == {"295050000000002": GID}


@pytest.mark.parametrize("imsi", ["123", "29505000000000x", "2950500000000011111"])
def test_enrol_bad_imsi_422(admin_headers, imsi):  # noqa: F811
    with _client() as c:
        assert c.post(f"/api/admin/soracom/sims/{imsi}/enrol", headers=admin_headers).status_code == 422


def test_enrol_all_skips_enrolled(admin_headers):  # noqa: F811
    fake = FakeSoracom(SIMS, groups=[{"groupId": GID, "tags": {"name": "pager-beam"}}])
    _use(fake)
    with _client() as c:
        r = c.post("/api/admin/soracom/enrol-all", headers=admin_headers)
    assert r.status_code == 200, r.text
    assert r.json() == {
        "ok": True,
        "enrolled": ["295050000000002", "295050000000003"],
        "already": ["295050000000001"],
    }
    assert set(fake.set_group) == {"295050000000002", "295050000000003"}


@pytest.mark.parametrize("status", [401, 500])
def test_soracom_failure_502(admin_headers, status):  # noqa: F811
    _use(FakeSoracom(SIMS, fail=status))
    with _client() as c:
        for r in (
            c.get("/api/admin/soracom/sims", headers=admin_headers),
            c.post("/api/admin/soracom/sims/295050000000001/enrol", headers=admin_headers),
            c.post("/api/admin/soracom/enrol-all", headers=admin_headers),
        ):
            assert r.status_code == 502
            assert r.json()["detail"] == f"soracom: HTTP {status}"
            assert "SECRET" not in r.text and "boom" not in r.text


def test_non_super_403():
    u = fb_auth.create_user(email="plain-sora@example.com")
    users_store.create_user(uid=u.uid, alias="plainsora", display_name="Plain")
    with _client() as c:
        h = auth_header(u.uid)
        assert c.get("/api/admin/soracom/sims", headers=h).status_code == 403
        assert c.post("/api/admin/soracom/enrol-all", headers=h).status_code == 403
        assert c.post("/api/admin/soracom/sims/295050000000001/enrol", headers=h).status_code == 403


def test_list_sims_pagination_and_optional_fields():
    sims = [{"imsi": str(i)} for i in range(5)] + [{"plan": 1, "tags": None}]
    fake = FakeSoracom(sims)
    _use(fake)
    with soracom.SoracomClient(KEY_ID, KEY) as client:
        rows = soracom.list_sims(client)
    assert len(rows) == 6
    assert rows[5].imsi is None and rows[5].subscription == "1" and rows[5].name is None
    assert [c for c in fake.calls if c[1] == "/subscribers"] == [("GET", "/subscribers")] * 3
    assert fake.calls.count(("POST", "/auth")) == 1
    assert not hasattr(rows[0], "msisdn")


def test_list_sims_caps_pages():
    def handler(req: httpx.Request) -> httpx.Response:
        if req.url.path.endswith("/auth"):
            return httpx.Response(200, json={"apiKey": "a", "token": "t"})
        return httpx.Response(200, json=[{"imsi": "1"}], headers={"X-Soracom-Next-Key": "more"})

    soracom._transport = httpx.MockTransport(handler)
    with soracom.SoracomClient(KEY_ID, KEY) as client:
        assert len(soracom.list_sims(client)) == soracom.MAX_PAGES


def test_error_text_has_no_secrets():
    def handler(req: httpx.Request) -> httpx.Response:
        if req.url.path.endswith("/auth"):
            return httpx.Response(403, text=f"{KEY_ID} {KEY}")
        raise AssertionError

    soracom._transport = httpx.MockTransport(handler)
    with soracom.SoracomClient(KEY_ID, KEY) as client, pytest.raises(soracom.SoracomError) as ei:
        soracom.list_sims(client)
    text = f"{ei.value} {ei.value.detail} {ei.value!r}"
    assert "SECRET" not in text and text.count("HTTP 403") >= 1

    def boom(req: httpx.Request) -> httpx.Response:
        raise httpx.ConnectError(f"cannot reach with {KEY}")

    soracom._transport = httpx.MockTransport(boom)
    with soracom.SoracomClient(KEY_ID, KEY) as client, pytest.raises(soracom.SoracomError) as ei:
        soracom.list_sims(client)
    assert ei.value.detail == "ConnectError" and "SECRET" not in repr(ei.value)

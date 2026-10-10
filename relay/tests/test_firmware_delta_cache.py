"""docs/OTA_DESIGN.md D12: on-demand deltas cached under `fw-cache/`. Real
tiny images and real detools; HTTP via `httpx.MockTransport`, the bucket write
via a fake storage client."""

from __future__ import annotations

import hashlib
import io
import json
import random
import zlib
from collections.abc import Iterator
from typing import Any

import detools
import httpx
import pytest
from fastapi.testclient import TestClient
from google.api_core.exceptions import PreconditionFailed

from app import firmware
from app.store import devices as devices_store
from tests.fake_transport import FakeBrokerClient
from tests.test_admin import (  # noqa: F401 -- fixtures
    _ota_device,
    admin_headers,
    broker,
    fake_emqx,
    make_settings,
)
from tests.test_firmware import BASE_URL, INDEX_URL, settings


def make_image(seed: int, n: int = 6000, patch: bytes = b"") -> bytes:
    rnd = random.Random(1)
    body = bytearray(rnd.randbytes(n))
    body[0] = 0xE9
    if seed:
        r2 = random.Random(seed)
        for _ in range(40):
            body[r2.randrange(1, n)] = r2.randrange(256)
    body += patch
    return bytes(body) + hashlib.sha256(bytes(body)).digest()


BASE_IMG = make_image(0)
NEW_IMG = make_image(7, patch=b"newcode" * 20)
BASE_ID, NEW_ID = BASE_IMG[-32:].hex(), NEW_IMG[-32:].hex()
B16, N16 = BASE_ID[:16], NEW_ID[:16]
CACHE_PATH = f"fw-cache/{N16}/from-{B16}.dz"
CACHE_URL = BASE_URL + CACHE_PATH


def _entry(img: bytes, version: str, published: int) -> dict:
    i16 = img[-32:].hex()[:16]
    z = zlib.compress(img, 9)
    return {
        "id": img[-32:].hex(), "version": version, "size": len(img), "published": published,
        "full": {"path": f"fw/{i16}/full.z", "osz": len(z), "osha": hashlib.sha256(z).hexdigest()},
        "deltas": [],
    }


def index_doc() -> dict:
    return {"v": 1, "builds": [_entry(NEW_IMG, "new", 2_000), _entry(BASE_IMG, "old", 1_000)]}


class FakeBlob:
    def __init__(self, store: FakeStorage, path: str) -> None:
        self.store, self.path = store, path
        self.metadata: dict[str, str] | None = None
        self.cache_control: str | None = None
        self.size = 0

    def upload_from_string(self, data: bytes, **kw: Any) -> None:
        self.store.uploads.append({"path": self.path, "data": data, "kw": kw, "meta": self.metadata,
                                   "cc": self.cache_control})
        if self.store.precondition_fail:
            raise PreconditionFailed("exists")
        if self.store.boom:
            raise RuntimeError("storage down")


class FakeStorage:
    def __init__(self) -> None:
        self.uploads: list[dict] = []
        self.precondition_fail = False
        self.boom = False
        self.existing: FakeBlob | None = None
        self.bucket_names: list[str] = []

    def bucket(self, name: str) -> FakeStorage:
        self.bucket_names.append(name)
        return self

    def blob(self, path: str) -> FakeBlob:
        return FakeBlob(self, path)

    def get_blob(self, path: str) -> FakeBlob | None:
        return self.existing


@pytest.fixture
def world() -> Iterator[dict]:
    w: dict[str, Any] = {
        "index": index_doc(), "cached": None, "head_status": None, "gets": [], "heads": 0,
        "storage": FakeStorage(), "corrupt_new": False,
    }

    def handler(request: httpx.Request) -> httpx.Response:
        url = str(request.url)
        if url == INDEX_URL:
            return httpx.Response(200, content=json.dumps(w["index"]).encode())
        if request.method == "HEAD":
            assert url == CACHE_URL, url
            w["heads"] += 1
            if w["head_status"] is not None:
                return httpx.Response(w["head_status"])
            if w["cached"] is None:
                return httpx.Response(404)
            dz, meta = w["cached"]
            return httpx.Response(
                200, headers={"content-length": str(len(dz)),
                              **{f"x-goog-meta-{k}": v for k, v in meta.items()}})
        w["gets"].append(url)
        for img in (BASE_IMG, NEW_IMG):
            if url == BASE_URL + f"fw/{img[-32:].hex()[:16]}/full.z":
                data = img
                if w["corrupt_new"] and img is NEW_IMG:
                    data = img[:100] + b"\x00" + img[101:]
                return httpx.Response(200, content=zlib.compress(data, 9))
        return httpx.Response(404)

    firmware.reset_cache()
    firmware.reset_delta_cache()
    firmware._transport = httpx.MockTransport(handler)
    firmware._storage_client_factory = lambda _s: w["storage"]
    yield w
    firmware._transport = None
    firmware._storage_client_factory = None
    firmware.reset_cache()
    firmware.reset_delta_cache()


def _index(w: dict) -> firmware.FirmwareIndex:
    return firmware.FirmwareIndex.model_validate(w["index"])


def _cached_object() -> tuple[bytes, dict[str, str]]:
    patch = io.BytesIO()
    detools.create_patch(io.BytesIO(BASE_IMG), io.BytesIO(NEW_IMG), patch,
                         patch_type="sequential", compression="none")
    raw = patch.getvalue()
    dz = zlib.compress(raw, 9)
    return dz, {"psz": str(len(raw)), "osha": hashlib.sha256(dz).hexdigest(), "base": BASE_ID,
                "img": NEW_ID}


def test_cached_object_found_no_upload(world):
    world["cached"] = _cached_object()
    d = firmware.delta_on_demand(_index(world), B16, N16, settings())
    assert d is not None and d.path == CACHE_PATH and d.base == BASE_ID
    assert d.osz == len(world["cached"][0]) and d.psz == int(world["cached"][1]["psz"])
    assert d.osha == world["cached"][1]["osha"]
    assert world["storage"].uploads == [] and world["gets"] == []


def test_not_cached_generates_verifies_and_uploads_once(world):
    d = firmware.delta_on_demand(_index(world), B16, N16, settings())
    assert d is not None and d.path == CACHE_PATH
    up = world["storage"].uploads
    assert len(up) == 1
    assert up[0]["path"] == CACHE_PATH and up[0]["kw"]["if_generation_match"] == 0
    assert up[0]["cc"] == "no-store"
    raw = zlib.decompress(up[0]["data"])
    assert up[0]["meta"] == {
        "psz": str(len(raw)), "osha": hashlib.sha256(up[0]["data"]).hexdigest(),
        "base": BASE_ID, "img": NEW_ID,
    }
    out = io.BytesIO()
    detools.apply_patch(io.BytesIO(BASE_IMG), io.BytesIO(raw), out)
    assert out.getvalue() == NEW_IMG
    assert (d.osz, d.psz, d.osha) == (len(up[0]["data"]), len(raw), up[0]["meta"]["osha"])
    assert world["storage"].bucket_names == ["fwbkt"]
    # now remembered: no second generation within the HEAD cache window
    assert firmware.cached_delta(_index(world), B16, N16, settings()) == d


def test_precondition_failed_uses_existing_object(world):
    st: FakeStorage = world["storage"]
    st.precondition_fail = True
    dz, meta = _cached_object()
    blob = FakeBlob(st, CACHE_PATH)
    blob.metadata, blob.size = meta, len(dz)
    st.existing = blob
    d = firmware.delta_on_demand(_index(world), B16, N16, settings())
    assert d is not None
    assert (d.osz, d.psz, d.osha) == (len(dz), int(meta["psz"]), meta["osha"])
    assert len(st.uploads) == 1


def test_base_not_in_index_is_full_without_fetch(world):
    idx = _index(world)
    assert firmware.delta_on_demand(idx, "9" * 16, N16, settings()) is None
    assert world["gets"] == [] and world["heads"] == 0 and world["storage"].uploads == []
    assert not firmware.can_delta_on_demand(idx, "9" * 16, N16)


def test_sha_mismatch_on_fetched_image_falls_back(world):
    world["corrupt_new"] = True
    assert firmware.delta_on_demand(_index(world), B16, N16, settings()) is None
    assert world["storage"].uploads == []


def test_head_500_treated_as_absent_then_generated(world):
    world["head_status"] = 500
    d = firmware.delta_on_demand(_index(world), B16, N16, settings())
    assert d is not None and len(world["storage"].uploads) == 1


def test_head_missing_header_treated_as_absent(world):
    dz, meta = _cached_object()
    del meta["osha"]
    world["cached"] = (dz, meta)
    assert firmware.cached_delta(_index(world), B16, N16, settings()) is None


def test_path_validator_accepts_cache_shape():
    firmware.FwDelta(path=CACHE_PATH, osz=10, osha="a" * 64, base=BASE_ID, psz=20)
    with pytest.raises(ValueError):
        firmware.FwDelta(path=f"fw-cache/{N16}/full.z", osz=10, osha="a" * 64, base=BASE_ID, psz=20)


# ---- admin integration ----


@pytest.fixture
def ota_client(world, fake_emqx, broker) -> Iterator[TestClient]:  # noqa: F811
    from app.main import create_app

    app = create_app(
        settings=make_settings(fw_index_url=INDEX_URL, fw_bucket_base=BASE_URL),
        broker_client=broker,
    )
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        yield c


def test_push_older_published_build_gets_on_demand_delta(
    ota_client, admin_headers, broker: FakeBrokerClient, world  # noqa: F811
):
    _ota_device(img=B16)
    r = ota_client.post("/api/admin/devices/pgr-ota-r/ota", json={"target": N16},
                        headers=admin_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["kind"] == "delta" and body["onDemand"] is True
    ota = json.loads(broker.published[-1].payload)["cfg"]["ota"]
    assert ota["fmt"] == "delta" and ota["url"] == CACHE_URL and ota["base"] == BASE_ID
    job = devices_store.get_device("pgr-ota-r").otaJob
    assert job.kind == "delta" and job.onDemand is True


def test_push_generation_exception_falls_back_to_full(
    ota_client, admin_headers, broker: FakeBrokerClient, world  # noqa: F811
):
    world["storage"].boom = True
    _ota_device(img=B16)
    r = ota_client.post("/api/admin/devices/pgr-ota-r/ota", json={"target": N16},
                        headers=admin_headers)
    assert r.status_code == 200, r.text
    assert r.json()["kind"] == "full" and "onDemand" not in r.json()
    ota = json.loads(broker.published[-1].payload)["cfg"]["ota"]
    assert ota["fmt"] == "full"
    assert devices_store.get_device("pgr-ota-r").otaJob.onDemand is False


def test_preview_never_generates_and_flags_on_demand(
    ota_client, admin_headers, world  # noqa: F811
):
    _ota_device(img=B16)
    builds = ota_client.get("/api/admin/firmware?device=pgr-ota-r", headers=admin_headers).json()[
        "builds"]
    new = next(b for b in builds if b["id16"] == N16)
    assert new["kind"] == "full" and new["onDemandDelta"] is True
    old = next(b for b in builds if b["id16"] == B16)
    assert "onDemandDelta" not in old
    assert world["storage"].uploads == [] and world["gets"] == []


def test_preview_reports_cached_delta(ota_client, admin_headers, world):  # noqa: F811
    world["cached"] = _cached_object()
    _ota_device(img=B16)
    builds = ota_client.get("/api/admin/firmware?device=pgr-ota-r", headers=admin_headers).json()[
        "builds"]
    new = next(b for b in builds if b["id16"] == N16)
    assert new["kind"] == "delta" and new["osz"] == len(world["cached"][0])
    assert world["storage"].uploads == []

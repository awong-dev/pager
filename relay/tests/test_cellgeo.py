"""Unit tests for `app.cellgeo` -- docs/PROTOCOL.md §13.2 / docs/SERVER_PLAN.md
§5.6 (cell-tower location fallback, this task): each provider's request/
response mapping (mocked HTTP), the Firestore cache (hit, 30-day expiry,
1-day negative cache), and the sanity checks (accuracy > 50km, exactly
0,0). Resolver failure not breaking `/webhooks/mqtt` ingest is covered
end-to-end in tests/test_location.py (a `/loc` answer must still get a 2xx
even when the provider call blows up).
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from typing import Any

import httpx
import pytest

from app import cellgeo
from app.db.firestore import get_db
from app.store import cells as cells_store
from app.wire import CellInfo


def _cell(**overrides: Any) -> CellInfo:
    obj: dict[str, Any] = {"mcc": "310", "mnc": "410", "tac": 12345, "ci": 87654321, "rsrp": -95}
    obj.update(overrides)
    return CellInfo.model_validate(obj)


@dataclass
class FakeResponse:
    status_code: int
    _json: dict | None = None
    text: str = ""

    def json(self) -> dict:
        if self._json is None:
            raise ValueError("no JSON body")
        return self._json


@pytest.fixture(autouse=True)
def _clean_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("CELL_GEO_PROVIDER", raising=False)
    monkeypatch.delenv("CELL_GEO_API_KEY", raising=False)


def _backdate(mcc: str, mnc: str, tac: int, ci: int, *, seconds_ago: int) -> None:
    key = cells_store.cache_key(mcc, mnc, tac, ci)
    ref = get_db().collection("cells").document(key)
    data = ref.get().to_dict()
    data["resolvedAt"] = datetime.now(UTC) - timedelta(seconds=seconds_ago)
    ref.set(data)


# ---------------------------------------------------------------------------
# CELL_GEO_PROVIDER=none / unconfigured
# ---------------------------------------------------------------------------


def test_provider_defaults_to_none():
    assert cellgeo.provider_name() == "none"


def test_resolve_is_a_noop_when_provider_is_none():
    assert cellgeo.resolve(_cell()) is None


def test_resolve_skips_the_lookup_when_key_is_missing(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    # CELL_GEO_API_KEY deliberately left unset.
    assert cellgeo.resolve(_cell()) is None


def test_resolve_never_raises_on_an_unknown_provider_name(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "not-a-real-provider")
    monkeypatch.setenv("CELL_GEO_API_KEY", "k")
    assert cellgeo.resolve(_cell()) is None


# ---------------------------------------------------------------------------
# google
# ---------------------------------------------------------------------------


def test_google_request_shape_and_response_mapping(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    captured: dict[str, Any] = {}

    def fake_post(url: str, *, params=None, json=None, timeout=None):
        captured["url"] = url
        captured["params"] = params
        captured["json"] = json
        return FakeResponse(200, {"location": {"lat": 1.5, "lng": 2.5}, "accuracy": 30})

    monkeypatch.setattr(cellgeo.httpx, "post", fake_post)

    fix = cellgeo.resolve(_cell())
    assert fix is not None
    assert fix.lat == 1.5
    assert fix.lon == 2.5
    assert fix.acc_m == 30
    assert fix.provider == "google"

    assert captured["url"] == cellgeo._GOOGLE_URL
    assert captured["params"] == {"key": "test-key"}
    # The whole point of considerIp: false: otherwise Google falls back to
    # the relay's own IP location, not the pager's.
    assert captured["json"]["considerIp"] is False
    assert captured["json"]["radioType"] == "lte"
    tower = captured["json"]["cellTowers"][0]
    assert tower == {
        "cellId": 87654321,
        "locationAreaCode": 12345,
        "mobileCountryCode": 310,
        "mobileNetworkCode": 410,
        "signalStrength": -95,
    }


def test_google_omits_signal_strength_when_rsrp_absent(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    captured: dict[str, Any] = {}

    def fake_post(url: str, *, params=None, json=None, timeout=None):
        captured["json"] = json
        return FakeResponse(200, {"location": {"lat": 1.0, "lng": 2.0}, "accuracy": 30})

    monkeypatch.setattr(cellgeo.httpx, "post", fake_post)
    cellgeo.resolve(_cell(ci=102, rsrp=None))
    assert "signalStrength" not in captured["json"]["cellTowers"][0]


def test_google_404_means_unknown_cell(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    monkeypatch.setattr(cellgeo.httpx, "post", lambda *a, **k: FakeResponse(404, {"error": {}}))
    assert cellgeo.resolve(_cell(ci=103)) is None


def test_google_non_2xx_returns_none(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    monkeypatch.setattr(cellgeo.httpx, "post", lambda *a, **k: FakeResponse(500, {}))
    assert cellgeo.resolve(_cell(ci=104)) is None


def test_google_transport_failure_never_raises(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")

    def boom(*a: Any, **k: Any):
        raise httpx.ConnectTimeout("simulated timeout")

    monkeypatch.setattr(cellgeo.httpx, "post", boom)
    assert cellgeo.resolve(_cell(ci=105)) is None


# ---------------------------------------------------------------------------
# opencellid (UNVERIFIED shape -- see app/cellgeo.py's docstring)
# ---------------------------------------------------------------------------


def test_opencellid_request_shape_and_response_mapping(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "opencellid")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    captured: dict[str, Any] = {}

    def fake_get(url: str, *, params=None, timeout=None):
        captured["url"] = url
        captured["params"] = params
        return FakeResponse(200, {"lat": 3.0, "lon": 4.0, "range": 500})

    monkeypatch.setattr(cellgeo.httpx, "get", fake_get)

    fix = cellgeo.resolve(_cell())
    assert fix is not None
    assert fix.lat == 3.0
    assert fix.lon == 4.0
    assert fix.acc_m == 500
    assert fix.provider == "opencellid"
    assert captured["url"] == cellgeo._OPENCELLID_URL
    assert captured["params"] == {
        "key": "test-key",
        "mcc": "310",
        "mnc": "410",
        "lac": 12345,
        "cellid": 87654321,
        "format": "json",
    }


def test_opencellid_unknown_cell_response(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "opencellid")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    monkeypatch.setattr(
        cellgeo.httpx, "get", lambda *a, **k: FakeResponse(200, {"error": "not found"})
    )
    assert cellgeo.resolve(_cell(ci=202)) is None


def test_opencellid_transport_failure_never_raises(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "opencellid")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")

    def boom(*a: Any, **k: Any):
        raise httpx.ConnectTimeout("simulated timeout")

    monkeypatch.setattr(cellgeo.httpx, "get", boom)
    assert cellgeo.resolve(_cell(ci=203)) is None


# ---------------------------------------------------------------------------
# Sanity checks -- this task's brief: "reject accuracy radii above 50 km and
# coordinates of exactly 0,0".
# ---------------------------------------------------------------------------


def test_sanity_rejects_accuracy_over_50km(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    monkeypatch.setattr(
        cellgeo.httpx,
        "post",
        lambda *a, **k: FakeResponse(200, {"location": {"lat": 1.0, "lng": 2.0}, "accuracy": 50_001}),
    )
    assert cellgeo.resolve(_cell(ci=301)) is None


def test_sanity_accepts_accuracy_at_exactly_50km(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    monkeypatch.setattr(
        cellgeo.httpx,
        "post",
        lambda *a, **k: FakeResponse(200, {"location": {"lat": 1.0, "lng": 2.0}, "accuracy": 50_000}),
    )
    assert cellgeo.resolve(_cell(ci=302)) is not None


def test_sanity_rejects_exactly_0_0(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    monkeypatch.setattr(
        cellgeo.httpx,
        "post",
        lambda *a, **k: FakeResponse(200, {"location": {"lat": 0.0, "lng": 0.0}, "accuracy": 30}),
    )
    assert cellgeo.resolve(_cell(ci=303)) is None


# ---------------------------------------------------------------------------
# Cache -- hit, 30-day expiry, 1-day negative cache.
# ---------------------------------------------------------------------------


def test_cache_hit_skips_a_second_http_call(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    calls = {"n": 0}

    def fake_post(*a: Any, **k: Any):
        calls["n"] += 1
        return FakeResponse(200, {"location": {"lat": 5.0, "lng": 6.0}, "accuracy": 100})

    monkeypatch.setattr(cellgeo.httpx, "post", fake_post)
    cell = _cell(ci=401)
    first = cellgeo.resolve(cell)
    second = cellgeo.resolve(cell)
    assert first == second
    assert calls["n"] == 1


def test_cache_expires_after_30_days(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    cell = _cell(ci=402)
    cells_store.remember_resolved(
        cell.mcc, cell.mnc, cell.tac, cell.ci, lat=1.0, lon=1.0, acc_m=10, provider="google"
    )
    _backdate(cell.mcc, cell.mnc, cell.tac, cell.ci, seconds_ago=cells_store.POSITIVE_TTL_S + 1)

    calls = {"n": 0}

    def fake_post(*a: Any, **k: Any):
        calls["n"] += 1
        return FakeResponse(200, {"location": {"lat": 9.0, "lng": 9.0}, "accuracy": 20})

    monkeypatch.setattr(cellgeo.httpx, "post", fake_post)
    fix = cellgeo.resolve(cell)
    assert fix is not None
    assert fix.lat == 9.0
    assert calls["n"] == 1


def test_negative_cache_skips_repeat_calls_within_a_day(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    calls = {"n": 0}

    def fake_post(*a: Any, **k: Any):
        calls["n"] += 1
        return FakeResponse(404, {})

    monkeypatch.setattr(cellgeo.httpx, "post", fake_post)
    cell = _cell(ci=403)
    assert cellgeo.resolve(cell) is None
    assert cellgeo.resolve(cell) is None
    assert calls["n"] == 1


def test_negative_cache_expires_after_1_day(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CELL_GEO_PROVIDER", "google")
    monkeypatch.setenv("CELL_GEO_API_KEY", "test-key")
    cell = _cell(ci=404)
    cells_store.remember_unknown(cell.mcc, cell.mnc, cell.tac, cell.ci, provider="google")
    _backdate(cell.mcc, cell.mnc, cell.tac, cell.ci, seconds_ago=cells_store.NEGATIVE_TTL_S + 1)

    calls = {"n": 0}

    def fake_post(*a: Any, **k: Any):
        calls["n"] += 1
        return FakeResponse(200, {"location": {"lat": 1.0, "lng": 1.0}, "accuracy": 10})

    monkeypatch.setattr(cellgeo.httpx, "post", fake_post)
    fix = cellgeo.resolve(cell)
    assert fix is not None
    assert calls["n"] == 1

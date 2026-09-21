"""Pluggable cell-tower-to-position resolver -- docs/PROTOCOL.md §13.2 /
docs/SERVER_PLAN.md §5.6 (cell-tower location fallback, this task).

GNSS and LTE cannot run at once on the pager's modem, and a school pager is
indoors most of the day, so a GNSS attempt usually ends in `no_fix`. The
pager always knows its serving cell (`mcc`/`mnc`/`tac`/`ci`, optionally
`rsrp`); it cannot turn that into coordinates, but the relay can, by asking a
third-party geolocation API. `app/location.py`'s `ingest_loc` is the only
caller.

**Privacy.** A lookup sends the *cell identity* to a third party -- never the
pager's own identity, never anything about the person carrying it, and never
more than the four/five numbers a `cell` sub-map carries. `CELL_GEO_PROVIDER
= "none"` (the default) is a real, supported choice: no lookup is ever made,
no third-party call happens, and nothing beyond `devices/{d}.status.lastCell`
(`app/store/devices.py`'s `set_last_cell`, called unconditionally by
`ingest_loc` whenever a `cell` arrives) is stored.

**Never raises into the ingest path.** `POST /webhooks/mqtt` must still
return 2xx even when a lookup fails, times out, or the provider is
misconfigured -- `resolve()` catches every exception from the HTTP call
itself and returns `None`, logging at WARNING without ever logging the API
key. One attempt, 5s timeout, no retry (a `/loc` answer is already on its way
to the requester with or without a resolved position).

**Cache-first.** `resolve()` checks `app/store/cells.py`'s 30-day (resolved)
/ 1-day (unknown) cache before ever calling a provider -- cells do not move,
the provider APIs bill per call, and the cache is what keeps cell-based
locations working through a provider outage.

Settings, read fresh from the environment on every call (the same per-call
`os.environ.get(...)` pattern `app/notify/sms.py`/`app/location.py` use, not
threaded through `app.config.Settings`):
  - `CELL_GEO_PROVIDER`: `"google"` | `"opencellid"` | `"none"` (default).
  - `CELL_GEO_API_KEY`: the provider's API key. Required for `google`/
    `opencellid`; if unset, `resolve()` logs a warning and behaves like
    `CELL_GEO_PROVIDER=none` for that call rather than crashing.
"""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass

import httpx

from app.store import cells as cells_store
from app.wire import CellInfo

logger = logging.getLogger("relay.cellgeo")

REQUEST_TIMEOUT_S = 5.0

# Sanity check on a resolved position (this task's brief): "reject accuracy
# radii above 50 km and coordinates of exactly 0,0" -- a bad/buggy provider
# response should never plant a pin in the middle of the ocean off West
# Africa (0,0) or claim a 50km+ radius fix is worth showing on a map at all.
MAX_ACCURACY_M = 50_000

_GOOGLE_URL = "https://www.googleapis.com/geolocation/v1/geolocate"
_OPENCELLID_URL = "https://opencellid.org/cell/get"


@dataclass(frozen=True, slots=True)
class CellFix:
    lat: float
    lon: float
    acc_m: int
    provider: str


def provider_name() -> str:
    return (os.environ.get("CELL_GEO_PROVIDER") or "none").strip().lower() or "none"


def api_key() -> str | None:
    return os.environ.get("CELL_GEO_API_KEY") or None


def _sane(fix: CellFix) -> bool:
    if fix.acc_m > MAX_ACCURACY_M:
        return False
    return not (fix.lat == 0.0 and fix.lon == 0.0)


def resolve(cell: CellInfo) -> CellFix | None:
    """`None` means "no position available" -- provider disabled/
    unconfigured, the provider does not know this cell, the request failed
    or timed out, or the response failed the sanity check. Never raises.
    Checks the cache first, and writes back to it (positive or negative)
    after a real provider call so a repeat lookup for the same cell within
    its TTL never touches the network at all."""
    name = provider_name()
    if name == "none":
        return None

    cached = cells_store.get_cached(cell.mcc, cell.mnc, cell.tac, cell.ci)
    if cached is not None:
        if cached.unknown:
            return None
        if cached.lat is not None and cached.lon is not None and cached.accM is not None:
            return CellFix(
                lat=cached.lat, lon=cached.lon, acc_m=cached.accM, provider=cached.provider or name
            )
        return None

    key = api_key()
    if not key:
        logger.warning(
            "CELL_GEO_PROVIDER=%s but CELL_GEO_API_KEY is unset; skipping cell lookup", name
        )
        return None

    try:
        if name == "google":
            fix = _resolve_google(cell, key)
        elif name == "opencellid":
            fix = _resolve_opencellid(cell, key)
        else:
            logger.warning("unknown CELL_GEO_PROVIDER=%r; skipping cell lookup", name)
            return None
    except Exception as exc:  # noqa: BLE001 -- must never break the /webhooks/mqtt ingest path
        logger.warning("cell geolocation lookup failed (provider=%s): %r", name, exc)
        return None

    if fix is None:
        cells_store.remember_unknown(cell.mcc, cell.mnc, cell.tac, cell.ci, provider=name)
        return None
    if not _sane(fix):
        logger.warning(
            "cell geolocation lookup returned an implausible fix (provider=%s, acc_m=%s, "
            "lat=%s, lon=%s); treated as unresolved",
            name,
            fix.acc_m,
            fix.lat,
            fix.lon,
        )
        cells_store.remember_unknown(cell.mcc, cell.mnc, cell.tac, cell.ci, provider=name)
        return None

    cells_store.remember_resolved(
        cell.mcc, cell.mnc, cell.tac, cell.ci, lat=fix.lat, lon=fix.lon, acc_m=fix.acc_m, provider=name
    )
    return fix


def _resolve_google(cell: CellInfo, key: str) -> CellFix | None:
    """`POST https://www.googleapis.com/geolocation/v1/geolocate?key=...`.
    `considerIp` **must** be `false`: unset (or true), Google falls back to
    the *caller's* (the relay's) IP-based location when the cell lookup
    itself is inconclusive, which would silently report the relay's own
    Cloud Run/hosting location as the child's -- putting them in a Google
    data centre, not their school. A 404 with an error body means "unknown
    cell" (not an error worth logging above INFO); any other non-2xx or a
    transport failure is a real failure, logged by the caller."""
    body: dict[str, object] = {
        "considerIp": False,
        "radioType": "lte",
        "cellTowers": [
            {
                "cellId": cell.ci,
                "locationAreaCode": cell.tac,
                "mobileCountryCode": int(cell.mcc),
                "mobileNetworkCode": int(cell.mnc),
                **({"signalStrength": cell.rsrp} if cell.rsrp is not None else {}),
            }
        ],
    }
    resp = httpx.post(_GOOGLE_URL, params={"key": key}, json=body, timeout=REQUEST_TIMEOUT_S)
    if resp.status_code == 404:
        return None  # "notFound": Google does not know this cell.
    if resp.status_code != 200:
        logger.warning("google geolocation returned status=%s", resp.status_code)
        return None
    data = resp.json()
    location = data.get("location") or {}
    lat, lon, acc = location.get("lat"), location.get("lng"), data.get("accuracy")
    if lat is None or lon is None or acc is None:
        return None
    return CellFix(lat=float(lat), lon=float(lon), acc_m=round(float(acc)), provider="google")


def _resolve_opencellid(cell: CellInfo, key: str) -> CellFix | None:
    """**UNVERIFIED** (this task's brief, no web access to confirm
    opencellid.org's current API shape at the time this was written):
    modelled on OpenCelliD's historic `cell/get` JSON endpoint -- `key`,
    `mcc`, `mnc`, `lac` (`=tac`), `cellid` (`=ci`), `format=json` ->
    `{lat, lon, range, ...}` on success, or a body with no `lat`/`lon` (an
    `"error"` key in some documented versions) when the cell is unknown.
    Verify against https://wiki.opencellid.org/wiki/api (or whatever has
    replaced it) before ever setting `CELL_GEO_PROVIDER=opencellid` in
    production -- implemented behind the same `resolve()` interface as
    `_resolve_google` so swapping the actual request/response shape in,
    if it has changed, needs no caller-side change."""
    resp = httpx.get(
        _OPENCELLID_URL,
        params={
            "key": key,
            "mcc": cell.mcc,
            "mnc": cell.mnc,
            "lac": cell.tac,
            "cellid": cell.ci,
            "format": "json",
        },
        timeout=REQUEST_TIMEOUT_S,
    )
    if resp.status_code != 200:
        logger.warning("opencellid returned status=%s", resp.status_code)
        return None
    try:
        data = resp.json()
    except ValueError:
        logger.warning("opencellid returned a non-JSON body")
        return None
    if not isinstance(data, dict) or "error" in data or "lat" not in data or "lon" not in data:
        return None  # unknown cell
    acc = data.get("range")
    return CellFix(
        lat=float(data["lat"]),
        lon=float(data["lon"]),
        acc_m=round(float(acc)) if acc is not None else MAX_ACCURACY_M,
        provider="opencellid",
    )

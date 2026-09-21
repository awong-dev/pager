"""`cells/{mcc}-{mnc}-{tac}-{ci}` -- cache of resolved cell-tower positions.

docs/PROTOCOL.md §13.2 / docs/SERVER_PLAN.md §5.6 (cell-tower location
fallback, this task): `app/cellgeo.py` calls a paid third-party API to turn a
pager's serving cell into a coarse position. Cells do not move, and the API
costs money per call, so every resolution (successful or "unknown cell") is
cached here, keyed by the cell identity itself -- content-addressed, the same
idiom `app/store/cas.py` uses for CA PEMs. A resolved position is reused for
**30 days**; an "unknown cell" answer is reused for only **1 day**, since a
provider's own coverage can grow and an unknown cell today may resolve
tomorrow. The cache is also what keeps cell-based locations working through a
provider outage.

Server-only: nothing in this collection is useful to a client directly (the
web app reads resolved fixes through `devices/{d}/locations`, not this
collection), so `relay/firestore.rules` gives it no `match` block at all --
same default-deny-with-no-match-block posture as `cas`/`deviceSecrets`/
`phoneIndex` (`relay/tests/test_rules.py` pins it, same as those three).
"""

from __future__ import annotations

from datetime import UTC, datetime

from google.cloud.firestore import SERVER_TIMESTAMP
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

# docs/PROTOCOL.md §13.2 (this task): "reuse them for 30 days; cache
# 'unknown cell' results for 1 day."
POSITIVE_TTL_S = 30 * 24 * 3600
NEGATIVE_TTL_S = 24 * 3600


class CellCacheEntry(BaseModel):
    """`unknown=True` is the negative-cache case (the provider was asked and
    said it does not know this cell) -- distinct from "never looked up"
    (`get_cached` returning `None`), so a negative cache hit does not retry
    the provider call until `NEGATIVE_TTL_S` has actually passed."""

    model_config = ConfigDict(extra="ignore")

    lat: float | None = None
    lon: float | None = None
    accM: int | None = None
    provider: str | None = None
    unknown: bool = False
    resolvedAt: datetime | None = None


def cache_key(mcc: str, mnc: str, tac: int, ci: int) -> str:
    return f"{mcc}-{mnc}-{tac}-{ci}"


def _cells():
    return get_db().collection("cells")


def get_cached(mcc: str, mnc: str, tac: int, ci: int) -> CellCacheEntry | None:
    """`None` means "no usable cache entry" -- either nothing was ever
    stored for this cell, or what is stored has aged past its class's TTL
    (30 days positive / 1 day negative) -- in either case the caller
    (`app/cellgeo.py`) should call the provider fresh. A present, fresh
    entry with `unknown=True` is itself a meaningful answer ("we already
    know this cell doesn't resolve, don't spend another API call")."""
    snap = _cells().document(cache_key(mcc, mnc, tac, ci)).get()
    if not snap.exists:
        return None
    entry = CellCacheEntry.model_validate(snap.to_dict() or {})
    if entry.resolvedAt is None:
        return None
    ttl = NEGATIVE_TTL_S if entry.unknown else POSITIVE_TTL_S
    age = (datetime.now(UTC) - entry.resolvedAt).total_seconds()
    if age >= ttl:
        return None
    return entry


def remember_resolved(
    mcc: str, mnc: str, tac: int, ci: int, *, lat: float, lon: float, acc_m: int, provider: str
) -> None:
    _cells().document(cache_key(mcc, mnc, tac, ci)).set(
        {
            "lat": lat,
            "lon": lon,
            "accM": acc_m,
            "provider": provider,
            "unknown": False,
            "resolvedAt": SERVER_TIMESTAMP,
        }
    )


def remember_unknown(mcc: str, mnc: str, tac: int, ci: int, *, provider: str) -> None:
    _cells().document(cache_key(mcc, mnc, tac, ci)).set(
        {
            "lat": None,
            "lon": None,
            "accM": None,
            "provider": provider,
            "unknown": True,
            "resolvedAt": SERVER_TIMESTAMP,
        }
    )

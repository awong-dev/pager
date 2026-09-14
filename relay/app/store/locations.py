"""`devices/{deviceId}/locations/{autoId}` -- docs/SERVER_PLAN.md §3, §5.6.

Ingest logic (dedup on wireId, `loc_req` fulfilment) lives in
`app/location.py`; this module is just the collection primitive it calls,
plus enough read support for the retention sweep and the admin API.
"""

from __future__ import annotations

from datetime import datetime

from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db


class LocationFix(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str | None = None
    ts: int
    fixTs: int
    lat: float
    lon: float
    accM: int | None = None
    src: str = "gnss"
    cached: bool = False
    reqId: str | None = None
    createdAt: datetime | None = None


def _locations(device_id: str):
    return get_db().collection("devices").document(device_id).collection("locations")


def locations_collection(device_id: str):
    """Public (unlike `_locations`) because `app/location.py` needs the raw
    collection reference itself -- to build a query it reads *inside its
    own* Firestore transaction (`Location.locate`'s <60s cached-fix check,
    docs/PROTOCOL.md §13.3 rule 6) and to pre-allocate a doc ref for a fix
    it writes *inside its own* dedup transaction (`ingest_loc`, §13.2) --
    neither of which this module's own non-transactional `add_location` can
    do."""
    return _locations(device_id)


def new_location_ref(device_id: str):
    """A fresh, unwritten doc ref (client-generated id, no round trip) in
    `devices/{device_id}/locations` -- for `app/location.py`'s `ingest_loc`,
    which needs to stage the fix write inside the same transaction as its
    `/loc` dedup marker (docs/PROTOCOL.md §13.2)."""
    return _locations(device_id).document()


def add_location(device_id: str, fix: LocationFix) -> str:
    from google.cloud.firestore import SERVER_TIMESTAMP

    doc = fix.model_dump(exclude={"id", "createdAt"})
    doc["createdAt"] = SERVER_TIMESTAMP
    _, ref = _locations(device_id).add(doc)
    return ref.id


def list_locations(device_id: str, *, limit: int = 50) -> list[LocationFix]:
    query = _locations(device_id).order_by("createdAt", direction="DESCENDING").limit(limit)
    return [
        LocationFix.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]


def latest_location(device_id: str) -> LocationFix | None:
    fixes = list_locations(device_id, limit=1)
    return fixes[0] if fixes else None

"""`families/{fid}/alerts/{id}` -- docs/FAMILIES_DESIGN.md §3 (the alerts
fields), §6 "Alert creation"; docs/FAMILIES_TASKS.md 3.3/4.1.

`create` is the low-level Firestore write (task 3.3's own "in this task
write the alert doc directly with the §3 fields" -- the Twilio webhook still
calls it, now indirectly through `app/alerts.py`'s `sms_unknown`, which also
pushes). `list_alerts`/`decide` (task 4.1) back `GET /api/family/alerts` and
the three `POST /api/family/alerts/{id}/{approve|block|dismiss}` routes in
`app/routers/family.py`. Named `list_alerts`, not `list` (docs/
FAMILIES_TASKS.md 4.1's shorthand), to match every other store module's own
`list_x` convention (`list_edges`, `list_users`, `list_requests`, ...) and
avoid shadowing the builtin.
"""

from __future__ import annotations

from datetime import UTC, datetime
from typing import Literal

from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

AlertKind = Literal["new_conversation", "sms_unknown", "contact_request"]
AlertStatus = Literal["open", "handled", "dismissed"]


class Alert(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    kind: AlertKind
    status: AlertStatus
    ts: datetime | None = None
    subjectUid: str | None = None
    subjectAlias: str | None = None
    peerUid: str | None = None
    peerAlias: str | None = None
    peerName: str | None = None
    peerPhone: str | None = None
    preview: str = ""
    heldBody: str | None = None
    convKey: str | None = None
    contactRequestKey: str | None = None
    decidedAt: datetime | None = None
    decidedBy: str | None = None


def _alerts(family_id: str):
    return get_db().collection("families").document(family_id).collection("alerts")


def create(family_id: str, alert: dict) -> str:
    """Writes `families/{family_id}/alerts/{id}` (Firestore-assigned id)
    with `alert`'s fields plus a server-timestamped `ts` -- docs/
    FAMILIES_DESIGN.md §3's exact alert shape (`kind`, `status`, `ts`,
    `subjectUid`, `subjectAlias`, `peerUid`, `peerAlias`, `peerPhone`,
    `preview`, `heldBody`, `convKey`, `contactRequestKey`, `decidedAt`,
    `decidedBy`). Callers pass every field `alert` needs except `ts`;
    returns the new document's id so the caller can pass it on to
    `app/backends/webapp.py`'s `push_alert`.

    Low-level primitive: prefer `app/alerts.py`'s `create` (which also
    pushes) over calling this directly -- `app/alerts.py`'s `new_conversation`
    / `sms_unknown` / `contact_request` are the only intended callers besides
    `app/alerts.py` itself and tests."""
    ref = _alerts(family_id).document()
    ref.set({**alert, "ts": SERVER_TIMESTAMP})
    return ref.id


def get(family_id: str, alert_id: str) -> Alert | None:
    snap = _alerts(family_id).document(alert_id).get()
    if not snap.exists:
        return None
    return Alert.model_validate({"id": alert_id, **(snap.to_dict() or {})})


def list_alerts(family_id: str, status: Literal["open", "all"] = "open") -> list[Alert]:
    """`status="open"` (the default, and `GET /api/family/alerts`'s own
    default) -- only `status == 'open'` docs; `"all"` -- every alert
    regardless of status. Newest first (`ts` descending, sorted in Python
    rather than via a composite Firestore index -- household/family scale,
    same choice `app/store/messages.py`'s `list_recent_queued_by_kind`
    documents for a similar bounded scan)."""
    query = _alerts(family_id)
    if status == "open":
        query = query.where(filter=FieldFilter("status", "==", "open"))
    alerts = [
        Alert.model_validate({"id": snap.id, **(snap.to_dict() or {})}) for snap in query.stream()
    ]
    alerts.sort(key=lambda a: a.ts or datetime.min.replace(tzinfo=UTC), reverse=True)
    return alerts


def decide(family_id: str, alert_id: str, status: Literal["handled", "dismissed"], by: str) -> Alert:
    """`status -> 'handled'|'dismissed'`, `decidedAt`/`decidedBy` set --
    the terminal transition every `POST /api/family/alerts/{id}/
    {approve|block|dismiss}` route ends with (`approve`/`block` decide
    `'handled'`, `dismiss` decides `'dismissed'`). Raises `KeyError` if
    `alert_id` does not exist in this family, matching `app/store/
    contacts.py`'s `approve`/`reject` contract."""
    ref = _alerts(family_id).document(alert_id)
    if not ref.get().exists:
        raise KeyError(f"no such alert: {family_id!r}/{alert_id!r}")
    ref.update({"status": status, "decidedAt": SERVER_TIMESTAMP, "decidedBy": by})
    fetched = get(family_id, alert_id)
    assert fetched is not None
    return fetched

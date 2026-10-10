"""`families/{fid}/alerts/{id}` -- docs/FAMILIES_DESIGN.md §3 (the alerts
fields), §6 "Alert creation"; docs/FAMILIES_TASKS.md 3.3/4.1.

`create` is the low-level Firestore write (task 3.3's own "in this task
write the alert doc directly with the §3 fields" -- reached through
`app/alerts.py`, which also pushes). `list_alerts`/`decide` (task 4.1) back `GET /api/family/alerts` and
the three `POST /api/family/alerts/{id}/{approve|block|dismiss}` routes in
`app/routers/family.py`. Named `list_alerts`, not `list` (docs/
FAMILIES_TASKS.md 4.1's shorthand), to match every other store module's own
`list_x` convention (`list_edges`, `list_users`, `list_requests`, ...) and
avoid shadowing the builtin.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from typing import Literal

from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction

# docs/BOOK_ADD_ANYONE_DESIGN.md D8: a decided approval alert stays decided
# (the kid is told "not approved") for this long, then a refused send reopens it.
APPROVAL_REOPEN_AFTER = timedelta(hours=24)
ApprovalOutcome = Literal["created", "open", "reopened", "declined"]

AlertKind = Literal["new_conversation", "sms_unknown", "contact_request", "chat_unknown"]
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
    # docs/RELAY_SMS_DESIGN.md decision 5: a held `sms_unknown` counts the
    # texts waiting; `updatedAt` moves with each new one.
    heldCount: int = 0
    updatedAt: datetime | None = None
    # docs/BRIDGE_PHONE_DESIGN.md decision 8: a `chat_unknown` alert names the
    # bridge conversation (`convRef` is what the web's Subscribe calls with).
    bridgeId: str | None = None
    conversationId: str | None = None
    convRef: str | None = None
    convTitle: str | None = None
    isGroup: bool | None = None
    people: list[str] = []
    source: str | None = None


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


def find_open(
    family_id: str,
    kind: AlertKind,
    subject_uid: str | None,
    peer_phone: str | None,
    bridge_conv: tuple[str, str] | None = None,
) -> Alert | None:
    """The open alert of `kind` for `(subject_uid, peer_phone)`, or `None`
    (the newest if, through a race, there are several). `bridge_conv` =
    `(bridgeId, conversationId)` additionally matches a `chat_unknown`
    alert's conversation (docs/BRIDGE_PHONE_DESIGN.md decision 8)."""
    query = _alerts(family_id).where(filter=FieldFilter("status", "==", "open"))
    found = [
        Alert.model_validate({"id": snap.id, **(snap.to_dict() or {})}) for snap in query.stream()
    ]
    found = [
        a
        for a in found
        if a.kind == kind
        and a.subjectUid == subject_uid
        and a.peerPhone == peer_phone
        and (bridge_conv is None or (a.bridgeId, a.conversationId) == bridge_conv)
    ]
    found.sort(key=lambda a: a.ts or datetime.min.replace(tzinfo=UTC), reverse=True)
    return found[0] if found else None


def update_fields(family_id: str, alert_id: str, fields: dict) -> None:
    """Merges `fields` plus a server `updatedAt` into an existing alert."""
    _alerts(family_id).document(alert_id).update({**fields, "updatedAt": SERVER_TIMESTAMP})


def get(family_id: str, alert_id: str) -> Alert | None:
    snap = _alerts(family_id).document(alert_id).get()
    if not snap.exists:
        return None
    return Alert.model_validate({"id": alert_id, **(snap.to_dict() or {})})


def list_alerts(family_id: str, status: Literal["open", "all"] = "open") -> list[Alert]:
    """`status="open"` (the default, and `GET /api/family/alerts`'s own
    default) -- only `status == 'open'` docs; `"all"` -- every alert
    regardless of status. Newest first (`ts` descending, sorted in Python
    rather than via a composite Firestore index -- household/family scale)."""
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


def approval_alert_id(owner_uid: str, peer_uid: str) -> str:
    return f"cr_{owner_uid}_{peer_uid}"


def upsert_approval(
    family_id: str, owner_uid: str, peer_uid: str, alert: dict, *, now: datetime | None = None
) -> ApprovalOutcome:
    """docs/BOOK_ADD_ANYONE_DESIGN.md D8: the `contact_request` alert
    `cr_{owner}_{peer}`, read and written in ONE transaction so a race cannot
    double-alert: absent -> create (`"created"`); `open` -> no-op (`"open"`);
    decided 24 h ago or more -> reopen (`"reopened"`, `ts` moves up);
    decided within 24 h -> `"declined"` (no write)."""
    now = now or datetime.now(UTC)
    ref = _alerts(family_id).document(approval_alert_id(owner_uid, peer_uid))

    def _txn(transaction: Transaction) -> ApprovalOutcome:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            transaction.create(ref, {**alert, "ts": SERVER_TIMESTAMP})
            return "created"
        data = snap.to_dict() or {}
        if data.get("status") == "open":
            return "open"
        decided_at = data.get("decidedAt")
        if isinstance(decided_at, datetime) and now - decided_at < APPROVAL_REOPEN_AFTER:
            return "declined"
        transaction.set(ref, {**alert, "ts": SERVER_TIMESTAMP})
        return "reopened"

    return run_transaction(_txn)

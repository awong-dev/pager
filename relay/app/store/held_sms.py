"""`heldSms/{MessageSid}` -- inbound texts from numbers the family has not
approved, kept until a parent decides (docs/RELAY_SMS_DESIGN.md decision 5).

Server-only (no `firestore.rules` match). The doc id is Twilio's
`MessageSid`, written with `create()`, so a Twilio retry is `AlreadyExists`
(`duplicate`) and approve can deliver each row with `wire_id=sid` dedup.
Queries are single-equality (`familyId`) with the rest filtered and sorted in
Python: household scale, no composite index.
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

HeldStatus = Literal["held", "delivered", "too_long", "blocked", "dismissed"]

# Cap per (familyId, fromPhone, toUid) of rows in status `held`.
HELD_CAP = 25
HELD_BODY_MAX_CODEPOINTS = 1600


class HeldSms(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    familyId: str
    toUid: str
    fromPhone: str
    body: str
    receivedAt: datetime
    status: HeldStatus = "held"
    decidedAt: datetime | None = None
    alertId: str | None = None


def _col():
    return get_db().collection("heldSms")


def create(
    sid: str, *, family_id: str, to_uid: str, from_phone: str, body: str, alert_id: str | None = None
) -> bool:
    """False when `sid` is already stored (a Twilio retry)."""
    try:
        _col().document(sid).create(
            {
                "familyId": family_id,
                "toUid": to_uid,
                "fromPhone": from_phone,
                "body": body[:HELD_BODY_MAX_CODEPOINTS],
                "receivedAt": SERVER_TIMESTAMP,
                "status": "held",
                "decidedAt": None,
                "alertId": alert_id,
            }
        )
    except AlreadyExists:
        return False
    return True


def exists(sid: str) -> bool:
    return _col().document(sid).get().exists


def get(sid: str) -> HeldSms | None:
    snap = _col().document(sid).get()
    if not snap.exists:
        return None
    return HeldSms.model_validate({"id": sid, **(snap.to_dict() or {})})


def list_held(
    family_id: str,
    from_phone: str,
    to_uid: str | None = None,
    *,
    status: HeldStatus | None = "held",
) -> list[HeldSms]:
    """Rows for `(family_id, from_phone, to_uid)` (`to_uid=None`: any
    recipient), oldest first, optionally filtered to one `status` (`None` =
    every status)."""
    rows = [
        HeldSms.model_validate({"id": s.id, **(s.to_dict() or {})})
        for s in _col().where(filter=FieldFilter("familyId", "==", family_id)).stream()
    ]
    rows = [
        r
        for r in rows
        if r.fromPhone == from_phone
        and (to_uid is None or r.toUid == to_uid)
        and (status is None or r.status == status)
    ]
    rows.sort(key=lambda r: (r.receivedAt, r.id))
    return rows


def count_held(family_id: str, from_phone: str, to_uid: str) -> int:
    return len(list_held(family_id, from_phone, to_uid, status="held"))


def set_status(ids: list[str], status: HeldStatus, alert_id: str | None = None) -> None:
    for sid in ids:
        updates: dict[str, object] = {"status": status, "decidedAt": SERVER_TIMESTAMP}
        if alert_id is not None:
            updates["alertId"] = alert_id
        _col().document(sid).update(updates)


def set_alert_id(ids: list[str], alert_id: str) -> None:
    for sid in ids:
        _col().document(sid).update({"alertId": alert_id})

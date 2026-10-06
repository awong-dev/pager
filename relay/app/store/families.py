"""`families/{fid}` -- docs/FAMILIES_DESIGN.md §3.

A family is the tenancy boundary: every `users/{uid}` and `devices/{d}`
belongs to exactly one (`familyId`), except externals, which have none
(§1 decision 6). This module owns only the `families/{fid}` document
itself -- membership lives on `users`/`devices`, not here.
"""

from __future__ import annotations

from datetime import datetime

from google.api_core.exceptions import NotFound
from google.cloud.firestore import SERVER_TIMESTAMP, ArrayUnion
from pydantic import BaseModel, ConfigDict

from app import wire
from app.db.firestore import get_db


class Family(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    name: str
    smsNumber: str | None = None
    blockedNumbers: list[str] = []
    createdAt: datetime | None = None
    createdBy: str | None = None


FAMILY_NAME_MAX_CODEPOINTS = 40


def validate_family_name(raw: str) -> str:
    """Strip; 1-40 code points; no control characters (docs/
    CONTACT_REQ_DESIGN.md decision 6). `ValueError` otherwise."""
    name = raw.strip()
    if not name:
        raise ValueError("family name must not be empty")
    if len(name) > FAMILY_NAME_MAX_CODEPOINTS:
        raise ValueError(f"family name exceeds {FAMILY_NAME_MAX_CODEPOINTS} characters")
    if wire.CONTROL_CHAR_RE.search(name):
        raise ValueError("family name contains control characters")
    return name


def _families():
    return get_db().collection("families")


def create_family(
    *,
    name: str,
    created_by: str,
    family_id: str | None = None,
    sms_number: str | None = None,
) -> Family:
    """Creates `families/{fid}`. `family_id` pins a specific id (the
    bootstrap's `default` family, tests, re-runs); omitted, Firestore
    assigns one."""
    ref = _families().document(family_id) if family_id else _families().document()
    ref.set(
        {
            "name": name,
            "smsNumber": sms_number,
            "blockedNumbers": [],
            "createdAt": SERVER_TIMESTAMP,
            "createdBy": created_by,
        }
    )
    fetched = get_family(ref.id)
    assert fetched is not None
    return fetched


def get_family(family_id: str) -> Family | None:
    snap = _families().document(family_id).get()
    if not snap.exists:
        return None
    return Family.model_validate({"id": family_id, **(snap.to_dict() or {})})


def list_families() -> list[Family]:
    return [
        Family.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in _families().stream()
    ]


def update_family(
    family_id: str, *, name: str | None = None, sms_number: str | None = None
) -> Family:
    """Patch-semantics update of `name`/`smsNumber` (`blockedNumbers` is
    mutated elsewhere, by the webhook's block action, not through here)."""
    updates: dict[str, object] = {}
    if name is not None:
        updates["name"] = name
    if sms_number is not None:
        updates["smsNumber"] = sms_number
    ref = _families().document(family_id)
    if updates:
        try:
            ref.update(updates)
        except NotFound as exc:
            raise KeyError(f"no such family: {family_id!r}") from exc
    fetched = get_family(family_id)
    if fetched is None:
        raise KeyError(f"no such family: {family_id!r}")
    return fetched


def add_blocked_number(family_id: str, phone: str) -> Family:
    """Adds `phone` to `families/{family_id}.blockedNumbers` (an `ArrayUnion`
    -- idempotent, a no-op if already blocked) -- docs/FAMILIES_DESIGN.md §4
    `/api/family/alerts/{id}/block` (docs/FAMILIES_TASKS.md 4.1), the "mutated
    elsewhere" this module's own docstring above anticipated."""
    ref = _families().document(family_id)
    try:
        ref.update({"blockedNumbers": ArrayUnion([phone])})
    except NotFound as exc:
        raise KeyError(f"no such family: {family_id!r}") from exc
    fetched = get_family(family_id)
    if fetched is None:
        raise KeyError(f"no such family: {family_id!r}")
    return fetched

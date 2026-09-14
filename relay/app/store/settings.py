"""`settings/retention` and `settings/meta` -- docs/SERVER_PLAN.md §3, §5.7.

`settings/meta.seqCounter` is the thread-ordering counter `messages.py`
increments inside the message-creating transaction; `get_and_init_meta`
exists so that increment never has to special-case "the document doesn't
exist yet" (bootstrap/tests would otherwise need to remember to seed it).
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import SERVER_TIMESTAMP
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

Unit = Literal["days", "weeks"]

DEFAULT_MESSAGES_RETENTION = {"n": 4, "unit": "weeks"}
DEFAULT_LOCATIONS_RETENTION = {"n": 1, "unit": "weeks"}


class RetentionSetting(BaseModel):
    model_config = ConfigDict(extra="ignore")

    n: int
    unit: Unit


class RetentionSettings(BaseModel):
    model_config = ConfigDict(extra="ignore")

    messages: RetentionSetting = RetentionSetting.model_validate(DEFAULT_MESSAGES_RETENTION)
    locations: RetentionSetting = RetentionSetting.model_validate(DEFAULT_LOCATIONS_RETENTION)


class MetaSettings(BaseModel):
    model_config = ConfigDict(extra="ignore")

    schemaVersion: int = 2
    lastSweepAt: datetime | None = None
    seqCounter: int = 0


def _settings():
    return get_db().collection("settings")


def get_retention() -> RetentionSettings:
    snap = _settings().document("retention").get()
    if not snap.exists:
        return RetentionSettings()
    return RetentionSettings.model_validate(snap.to_dict() or {})


def set_retention(*, messages: RetentionSetting, locations: RetentionSetting) -> RetentionSettings:
    _settings().document("retention").set(
        {"messages": messages.model_dump(), "locations": locations.model_dump()}
    )
    fetched = get_retention()
    return fetched


def get_meta() -> MetaSettings:
    snap = _settings().document("meta").get()
    if not snap.exists:
        return MetaSettings()
    return MetaSettings.model_validate(snap.to_dict() or {})


def mark_swept() -> None:
    """`settings/meta.lastSweepAt = now` -- called once at the end of a
    successful `app.jobs.sweep()` run (docs/SERVER_PLAN.md §3's schema
    already reserves this field; nothing wrote it before this phase). Uses
    `set(..., merge=True)` rather than `update()` so this is safe to call
    even if `settings/meta` doesn't exist yet (a sweep with nothing to do,
    on a brand-new deployment, must not crash for want of
    `ensure_meta_initialized()` having run first)."""
    _settings().document("meta").set({"lastSweepAt": SERVER_TIMESTAMP}, merge=True)


def ensure_meta_initialized() -> MetaSettings:
    """Idempotently creates `settings/meta` with defaults if absent. Safe to
    call repeatedly (bootstrap, app startup, tests)."""
    ref = _settings().document("meta")
    try:
        # `create()` (not exists-check + `set()`): a concurrent
        # `messages.create_message` may be creating this very document
        # inside its transaction, and a racing `set()` would reset
        # `seqCounter` to 0 and hand out a duplicate `seq`.
        ref.create(MetaSettings().model_dump())
    except AlreadyExists:
        pass
    return get_meta()

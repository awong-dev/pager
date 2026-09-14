"""`users/{uid}/backends/{bid}` -- docs/SERVER_PLAN.md §3, §6.1.

Fan-out target for routing (Phase 3); this module only owns CRUD. `config`
is adapter-specific (§6.1's `config_schema` per kind) and is stored as a
plain dict here -- validating it against a specific backend's schema is that
backend module's job, not the store's.
"""

from __future__ import annotations

import uuid
from datetime import datetime
from typing import Literal

from google.cloud.firestore import SERVER_TIMESTAMP
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

BackendKind = Literal["pager", "webapp", "sms", "gchat"]


class Backend(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    kind: BackendKind
    config: dict = {}
    enabled: bool = True
    verifiedAt: datetime | None = None


def _backends(uid: str):
    return get_db().collection("users").document(uid).collection("backends")


def create_backend(
    uid: str, *, kind: BackendKind, config: dict | None = None, enabled: bool = True
) -> Backend:
    bid = uuid.uuid4().hex[:12]
    ref = _backends(uid).document(bid)
    ref.set(
        {
            "kind": kind,
            "config": config or {},
            "enabled": enabled,
            "verifiedAt": SERVER_TIMESTAMP if kind == "webapp" else None,
        }
    )
    fetched = get_backend(uid, bid)
    assert fetched is not None
    return fetched


def get_backend(uid: str, bid: str) -> Backend | None:
    snap = _backends(uid).document(bid).get()
    if not snap.exists:
        return None
    return Backend.model_validate({"id": bid, **(snap.to_dict() or {})})


def list_backends(uid: str) -> list[Backend]:
    return [
        Backend.model_validate({"id": snap.id, **(snap.to_dict() or {})})
        for snap in _backends(uid).stream()
    ]


def update_backend(
    uid: str,
    bid: str,
    *,
    config: dict | None = None,
    enabled: bool | None = None,
    verified: bool = False,
) -> Backend:
    updates: dict[str, object] = {}
    if config is not None:
        updates["config"] = config
    if enabled is not None:
        updates["enabled"] = enabled
    if verified:
        updates["verifiedAt"] = SERVER_TIMESTAMP
    if updates:
        _backends(uid).document(bid).update(updates)
    fetched = get_backend(uid, bid)
    if fetched is None:
        raise KeyError(f"no such backend: {uid}/{bid}")
    return fetched


def delete_backend(uid: str, bid: str) -> None:
    _backends(uid).document(bid).delete()

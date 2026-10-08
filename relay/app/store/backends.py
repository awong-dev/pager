"""`users/{uid}/backends/{bid}` -- docs/SERVER_PLAN.md §3, §6.1.

Fan-out target for `app/routing.py`; this module only owns CRUD. `config`
is adapter-specific (§6.1's `config_schema` per kind) and is stored as a
plain dict here -- validating it against a specific backend's schema is that
backend module's job, not the store's.

Two small top-level lookup collections, neither named in §3's schema table,
exist so the gchat inbound webhook (`app/routers/webhooks.py`,
docs/SERVER_PLAN.md §6.5) can map "a link code someone just typed into
Chat" / "a Chat space that just messaged us" back to `(uid, bid)` with a
single Firestore `get()` rather than a collection-group scan (Firestore's
automatic single-field indexing does not cover `COLLECTION_GROUP`-scoped
queries on a *nested* `config.*` field without an explicit
`firestore.indexes.json` entry -- these collections sidestep that entirely,
the same "the doc id is the uniqueness/lookup key" trick `aliases/{alias}`
already uses in §3):

- `gchatLinkCodes/{code}` -> `{uid, bid, expiresAt}` -- written by
  `GChatBackend.start_link()`, popped (read-then-delete, single use) by the
  inbound `/link CODE` handler.
- `gchatSpaces/{spaceId}` -> `{uid, bid, senderName}` -- written once a
  `/link` message resolves, so every *later* message in that DM space maps
  straight back to the linked backend. `spaceId` is the last path segment of
  Chat's `spaces/AAAAAAAAAA` resource name (a raw `/`-containing resource
  name cannot be a Firestore document id). `senderName` is Chat's
  `message.sender.name` for the `/link` message itself -- stashed so a later
  message in the same space can be checked against it (§6.5 assumes a 1:1
  DM; nothing about the space object itself guarantees that stays true, so
  the *sender* is pinned at link time and re-checked on every subsequent
  message, `app/routers/webhooks.py`'s `/webhooks/gchat`).

`kind:"sms"` (docs/RELAY_SMS_DESIGN.md decision 3) is valid only on an
external (`users/{uid}.kind == "external"`): a `sms` row on a person -- the
7 Oct 2026 person-phone rows still in prod -- is skipped by `get_backend`/
`list_backends`, as is any row of a retired kind (it must never 500 a read).
"""

from __future__ import annotations

import logging
import uuid
from datetime import datetime
from typing import Literal, get_args

from google.cloud.firestore import SERVER_TIMESTAMP
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

logger = logging.getLogger(__name__)

BackendKind = Literal["pager", "webapp", "gchat", "sms"]


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
    uid: str,
    *,
    kind: BackendKind,
    config: dict | None = None,
    enabled: bool = True,
    bid: str | None = None,
) -> Backend:
    """`bid` pins the document id (idempotent `set`); default is a random one."""
    bid = bid or uuid.uuid4().hex[:12]
    ref = _backends(uid).document(bid)
    ref.set(
        {
            "kind": kind,
            "config": config or {},
            "enabled": enabled,
            "verifiedAt": SERVER_TIMESTAMP if kind in ("webapp", "sms") else None,
        }
    )
    fetched = get_backend(uid, bid)
    assert fetched is not None
    return fetched


def _owner_is_external(uid: str) -> bool:
    snap = get_db().collection("users").document(uid).get()
    return snap.exists and (snap.to_dict() or {}).get("kind") == "external"


def _live_kind(uid: str, bid: str, data: dict, external: bool | None = None) -> bool:
    kind = data.get("kind")
    if kind not in get_args(BackendKind):
        logger.warning("skipping backend %s/%s of retired kind %r", uid, bid, kind)
        return False
    if kind == "sms":
        if external is None:
            external = _owner_is_external(uid)
        if not external:
            logger.warning("skipping sms backend %s/%s on a non-external owner", uid, bid)
            return False
    return True


def get_backend(uid: str, bid: str) -> Backend | None:
    snap = _backends(uid).document(bid).get()
    if not snap.exists:
        return None
    data = snap.to_dict() or {}
    if not _live_kind(uid, bid, data):
        return None
    return Backend.model_validate({"id": bid, **data})


def list_backends(uid: str) -> list[Backend]:
    out: list[Backend] = []
    external: bool | None = None  # looked up at most once, only if an sms row is seen
    for snap in _backends(uid).stream():
        data = snap.to_dict() or {}
        if data.get("kind") == "sms" and external is None:
            external = _owner_is_external(uid)
        if _live_kind(uid, snap.id, data, external):
            out.append(Backend.model_validate({"id": snap.id, **data}))
    return out


def update_backend(
    uid: str,
    bid: str,
    *,
    config: dict | None = None,
    enabled: bool | None = None,
    verified: bool | None = None,
) -> Backend:
    """`verified`: `None` (default) leaves `verifiedAt` untouched, `True`
    sets it to now, `False` **explicitly clears it back to `None`** -- not
    just "no-op", unlike `config`/`enabled`'s `None`-means-skip convention."""
    updates: dict[str, object] = {}
    if config is not None:
        updates["config"] = config
    if enabled is not None:
        updates["enabled"] = enabled
    if verified is True:
        updates["verifiedAt"] = SERVER_TIMESTAMP
    elif verified is False:
        updates["verifiedAt"] = None
    if updates:
        _backends(uid).document(bid).update(updates)
    fetched = get_backend(uid, bid)
    if fetched is None:
        raise KeyError(f"no such backend: {uid}/{bid}")
    return fetched


def delete_backend(uid: str, bid: str) -> None:
    _backends(uid).document(bid).delete()


# ---------------------------------------------------------------------------
# lookup collections -- see this module's docstring
# ---------------------------------------------------------------------------


def _gchat_link_codes():
    return get_db().collection("gchatLinkCodes")


def set_gchat_link_code(code: str, uid: str, bid: str, expires_at: int) -> None:
    """Called by `GChatBackend.start_link()` -- `expires_at` is a Unix
    timestamp, checked (not enforced by Firestore TTL, to stay consistent
    with the rest of this project's "derived expiry" style, §5.6) by the
    inbound `/link` handler before it honours the code."""
    _gchat_link_codes().document(code).set({"uid": uid, "bid": bid, "expiresAt": expires_at})


def pop_gchat_link_code(code: str) -> tuple[str, str, int] | None:
    """Read-then-delete (single use): returns `(uid, bid, expiresAt)` for
    `code`, or `None` if no such code was ever issued (already used, or
    never existed). The caller is responsible for checking `expiresAt`
    against the current time -- popping here regardless means a stale code
    can never be replayed even after it has expired."""
    ref = _gchat_link_codes().document(code)
    snap = ref.get()
    if not snap.exists:
        return None
    data = snap.to_dict() or {}
    ref.delete()
    uid, bid, expires_at = data.get("uid"), data.get("bid"), data.get("expiresAt")
    if not uid or not bid or expires_at is None:
        return None
    return uid, bid, expires_at


def _gchat_spaces():
    return get_db().collection("gchatSpaces")


def set_gchat_space(space_id: str, uid: str, bid: str, sender_name: str | None = None) -> None:
    """`sender_name` (M2): Chat's `message.sender.name` for the `/link`
    message that established this link -- `None` only for callers (existing
    tests, pre-fix data) that never pinned one; `get_by_gchat_space`'s
    caller treats a `None` `senderName` as "unknown, do not enforce" rather
    than a hard mismatch, so this stays backwards compatible."""
    _gchat_spaces().document(space_id).set(
        {"uid": uid, "bid": bid, "senderName": sender_name}
    )


def get_by_gchat_space(space_id: str) -> tuple[str, str, str | None] | None:
    """`(uid, bid, senderName)` of the gchat backend linked to Chat space
    `space_id` (the last path segment of `spaces/AAAAAAAAAA`), or `None`.
    `senderName` is the sender pinned at link time (M2) -- the caller
    (`app/routers/webhooks.py`'s `/webhooks/gchat`) is responsible for
    checking a later message's sender against it before treating the
    message as coming from the linked user."""
    snap = _gchat_spaces().document(space_id).get()
    if not snap.exists:
        return None
    data = snap.to_dict() or {}
    uid, bid = data.get("uid"), data.get("bid")
    if not uid or not bid:
        return None
    return uid, bid, data.get("senderName")

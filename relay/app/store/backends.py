"""`users/{uid}/backends/{bid}` -- docs/SERVER_PLAN.md §3, §6.1.

Fan-out target for `app/routing.py`; this module only owns CRUD. `config`
is adapter-specific (§6.1's `config_schema` per kind) and is stored as a
plain dict here -- validating it against a specific backend's schema is that
backend module's job, not the store's.

Three small top-level lookup collections,
none named in §3's schema table, exist so the sms/gchat inbound webhooks
(`app/routers/webhooks.py`, docs/SERVER_PLAN.md §6.4/§6.5) can map "a phone
number that just texted us" / "a Chat space that just messaged us" / "a
link code someone just typed into Chat" back to `(uid, bid)` with a single
Firestore `get()` rather than a collection-group scan (Firestore's
automatic single-field indexing does not cover `COLLECTION_GROUP`-scoped
queries on a *nested* `config.*` field without an explicit
`firestore.indexes.json` entry -- these three collections sidestep that
entirely, the same "the doc id is the uniqueness/lookup key" trick
`aliases/{alias}` already uses in §3):

- `phoneIndex/{e164Phone}` -> `{uid, bid}` -- written once an sms backend's
  phone is verified (`app/routers/me.py`'s verify route), *not* at creation
  time, so an unverified/never-completed phone claim can never be used to
  steal another user's inbound texts.
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

A fourth top-level lookup collection has the
same posture as the three above (no `firestore.rules` `match` block ->
default-deny client read):

- `smsVerifyCodes/{bid}` -> `{codeHash, expiresAt}` -- written by
  `SmsTwilioBackend.start_link()`, checked by `complete_link()`. Deliberately
  **not** `backend.config` (which is client-readable by the backend's own
  owner, `firestore.rules`' `users/{uid}/backends/{b}` rule) -- a phone
  number's real owner is exactly the person a claimant of that number needs
  to be verified *against*, so the code must live somewhere the claimant
  cannot read it back out of Firestore themselves. `codeHash`, not the raw
  code, so even a rules regression that started exposing this collection
  would not hand out a usable code directly (a 6-digit code is still
  brute-forceable offline from a hash if an attacker can enumerate this
  collection -- the no-`match`-block default-deny posture is the real
  control; hashing is defense in depth, not a substitute for it). This is
  *not* the same situation as `gchatLinkCodes`/`linkCode` above/`config.
  linkCode`, which are deliberately owner-readable: the Chat link code is
  shown *to the user being linked* so they can type it into a Chat DM they
  already control, whereas an sms verify code must never be readable by the
  person whose ownership of the phone is in question.

This is additive to §3's table, not a contradiction of it -- the plan
specifies the *behaviour* ("map `From` -> user by verified phone",
"stores the DM `space` name") but not the index mechanism.
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
    verified: bool | None = None,
) -> Backend:
    """`verified`: `None` (default) leaves `verifiedAt` untouched, `True`
    sets it to now, `False` **explicitly clears it back to `None`** -- not
    just "no-op", unlike `config`/`enabled`'s `None`-means-skip convention.
    Needed by `app/routers/me.py`'s `PATCH /api/me/backends/{bid}` (H3): a
    backend whose `config.phone` is being changed away from its previously-
    verified number must lose `verifiedAt`, or a re-pointed sms backend stays
    "verified" for a number it was never checked against."""
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


def _phone_index():
    return get_db().collection("phoneIndex")


def set_phone_index(phone: str, uid: str, bid: str) -> None:
    """Called once an sms backend's phone is verified -- see this module's
    docstring for why this is written on *verify*, not on backend
    creation."""
    _phone_index().document(phone).set({"uid": uid, "bid": bid})


def get_by_phone(phone: str) -> tuple[str, str] | None:
    """`(uid, bid)` of the sms backend whose verified phone is `phone`, or
    `None` -- `app/routers/webhooks.py`'s inbound Twilio webhook uses this
    to map `From` to a user (docs/SERVER_PLAN.md §6.4)."""
    snap = _phone_index().document(phone).get()
    if not snap.exists:
        return None
    data = snap.to_dict() or {}
    uid, bid = data.get("uid"), data.get("bid")
    if not uid or not bid:
        return None
    return uid, bid


def clear_phone_index(phone: str) -> None:
    _phone_index().document(phone).delete()


def _sms_verify_codes():
    return get_db().collection("smsVerifyCodes")


def set_sms_verify_code(bid: str, code_hash: str, expires_at: int) -> None:
    """Called by `SmsTwilioBackend.start_link()` -- see this module's
    docstring for why this is a server-only top-level collection rather than
    `backend.config` (H1: `config` is owner-readable, which would let the
    person being challenged read their own verification code straight out
    of Firestore)."""
    _sms_verify_codes().document(bid).set({"codeHash": code_hash, "expiresAt": expires_at})


def get_sms_verify_code(bid: str) -> tuple[str, int] | None:
    """`(codeHash, expiresAt)` for backend `bid`'s pending sms verification,
    or `None` if none is pending -- `SmsTwilioBackend.complete_link()`'s
    proof check reads from here, never from `backend.config`."""
    snap = _sms_verify_codes().document(bid).get()
    if not snap.exists:
        return None
    data = snap.to_dict() or {}
    code_hash, expires_at = data.get("codeHash"), data.get("expiresAt")
    if not code_hash or expires_at is None:
        return None
    return code_hash, expires_at


def clear_sms_verify_code(bid: str) -> None:
    _sms_verify_codes().document(bid).delete()


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

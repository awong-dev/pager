"""`users/{uid}` and `aliases/{alias}` -- docs/SERVER_PLAN.md §3.

`aliases/{alias}` exists purely so the alias is unique (the doc id *is* the
uniqueness constraint) and so the wire (`from`/`to`, PROTOCOL.md §3.1) can be
resolved to a uid without a query. It is created in the same transaction as
the `users/{uid}` document it points at, so the two can never disagree.
"""

from __future__ import annotations

import re
from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists, NotFound
from google.cloud.firestore import SERVER_TIMESTAMP, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction
from app.store import backends as backends_store

# Same shape as PROTOCOL.md §3.1's alias regex (from/to on the wire).
ALIAS_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,15}$")
RESERVED_ALIASES = {"system"}

Role = Literal["admin", "member"]


class User(BaseModel):
    model_config = ConfigDict(extra="ignore")

    uid: str
    alias: str
    displayName: str
    email: str | None = None
    phone: str | None = None
    role: Role = "member"
    disabled: bool = False
    createdAt: datetime | None = None


class AliasTaken(Exception):
    """`alias` is already registered to a different (or the same, on
    create) uid."""


class InvalidAlias(Exception):
    pass


def _users() -> object:
    return get_db().collection("users")


def _aliases() -> object:
    return get_db().collection("aliases")


def _validate_alias(alias: str) -> None:
    if alias in RESERVED_ALIASES or not ALIAS_RE.match(alias):
        raise InvalidAlias(f"invalid or reserved alias: {alias!r}")


def create_user(
    *,
    uid: str,
    alias: str,
    display_name: str,
    email: str | None = None,
    phone: str | None = None,
    role: Role = "member",
) -> User:
    """Creates `users/{uid}` and `aliases/{alias}` in one transaction --
    `aliases/{alias}` is created with `transaction.create`, which raises
    `AlreadyExists` (re-raised here as `AliasTaken`) if the alias is already
    taken, so the two documents can never be created out of sync."""
    _validate_alias(alias)
    db = get_db()
    user_ref = db.collection("users").document(uid)
    alias_ref = db.collection("aliases").document(alias)

    def _txn(transaction: Transaction) -> None:
        transaction.create(alias_ref, {"uid": uid})
        transaction.create(
            user_ref,
            {
                "alias": alias,
                "displayName": display_name,
                "email": email,
                "phone": phone,
                "role": role,
                "disabled": False,
                "createdAt": SERVER_TIMESTAMP,
            },
        )

    try:
        run_transaction(_txn)
    except AlreadyExists as exc:
        raise AliasTaken(f"alias already registered: {alias!r}") from exc

    # docs/SERVER_PLAN.md §6.3: "Every user gets an implicit webapp backend
    # at creation." Not part of the uid/alias transaction above (a backend
    # doc failing to write must not roll back a user that's otherwise fine
    # -- app/routers/me.py's backend listing is written to tolerate its
    # absence too, matching import_sqlite.py's existing non-transactional
    # pattern for the same backend).
    backends_store.create_backend(uid, kind="webapp", config={}, enabled=True)

    fetched = get_user(uid)
    assert fetched is not None
    return fetched


def get_user(uid: str) -> User | None:
    snap = get_db().collection("users").document(uid).get()
    if not snap.exists:
        return None
    return User.model_validate({"uid": uid, **(snap.to_dict() or {})})


def get_uid_for_alias(alias: str) -> str | None:
    snap = get_db().collection("aliases").document(alias).get()
    if not snap.exists:
        return None
    data = snap.to_dict() or {}
    return data.get("uid")


def get_user_by_alias(alias: str) -> User | None:
    uid = get_uid_for_alias(alias)
    if uid is None:
        return None
    return get_user(uid)


def list_users() -> list[User]:
    return [
        User.model_validate({"uid": snap.id, **(snap.to_dict() or {})})
        for snap in get_db().collection("users").stream()
    ]


def update_user(
    uid: str,
    *,
    display_name: str | None = None,
    email: str | None = None,
    phone: str | None = None,
    role: Role | None = None,
    disabled: bool | None = None,
) -> User:
    """Patch-semantics update of mutable fields. `alias` is intentionally
    not editable here -- changing it would orphan the old `aliases/{alias}`
    doc or require another transaction; not needed by any caller yet."""
    updates: dict[str, object] = {}
    if display_name is not None:
        updates["displayName"] = display_name
    if email is not None:
        updates["email"] = email
    if phone is not None:
        updates["phone"] = phone
    if role is not None:
        updates["role"] = role
    if disabled is not None:
        updates["disabled"] = disabled
    ref = get_db().collection("users").document(uid)
    if updates:
        try:
            ref.update(updates)
        except NotFound as exc:
            raise KeyError(f"no such user: {uid!r}") from exc
    fetched = get_user(uid)
    if fetched is None:
        raise KeyError(f"no such user: {uid!r}")
    return fetched


def delete_user(uid: str) -> None:
    """Deletes `users/{uid}` and its `aliases/{alias}` doc together. Does
    NOT cascade to backends/devices/messages/allow edges -- that is a wider
    admin operation left to a later phase; this is the primitive."""
    db = get_db()
    user = get_user(uid)
    user_ref = db.collection("users").document(uid)

    if user is not None:
        alias_ref = db.collection("aliases").document(user.alias)

        def _txn(transaction: Transaction) -> None:
            transaction.delete(alias_ref)
            transaction.delete(user_ref)

        run_transaction(_txn)
    else:
        user_ref.delete()

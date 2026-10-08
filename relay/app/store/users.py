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
from pydantic import BaseModel, ConfigDict, Field

from app.db.firestore import get_db, run_transaction
from app.store import backends as backends_store

# Same shape as PROTOCOL.md §3.1's alias regex (from/to on the wire).
ALIAS_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,15}$")
RESERVED_ALIASES = {"system"}

# docs/FAMILIES_DESIGN.md §1 decision 2: "super" is the global superadmin,
# "admin" is now a *family* admin.
Role = Literal["super", "admin", "member"]
Kind = Literal["person", "external"]

# docs/FAMILIES_DESIGN.md §2: the two policy pickers' machine codes. Kept as
# plain `str` here (not restricted to these literals) because `app/
# policy.py` (task 3.1) is the module that owns validating them against
# `OUT`/`IN` -- this module only needs a shape to store the default in.


class Policy(BaseModel):
    """`users/{uid}.policy` -- docs/FAMILIES_DESIGN.md §2. `in` is a Python
    keyword, hence `in_`/`Field(alias="in")`; `populate_by_name` lets
    callers construct one with either name, while `model_validate` (used
    when reading the Firestore dict back, which has a literal `"in"` key)
    always works via the alias regardless."""

    model_config = ConfigDict(populate_by_name=True)

    out: str
    in_: str = Field(alias="in")


class Notify(BaseModel):
    """`users/{uid}.notify` -- docs/FAMILIES_DESIGN.md §3. Only one flag so
    far: whether this user's family-admin alerts are pushed (task 4.2)."""

    alerts: bool = True


# docs/FAMILIES_DESIGN.md §2, last paragraph: "Defaults are applied at user
# creation from role (`member` -> `people`/`people`, `admin`/`super` ->
# `open`/`any`)".
_DEFAULT_MEMBER_POLICY = {"out": "people", "in": "people"}
_DEFAULT_ADMIN_POLICY = {"out": "open", "in": "any"}


def _default_policy(role: Role) -> dict[str, str]:
    return dict(_DEFAULT_MEMBER_POLICY if role == "member" else _DEFAULT_ADMIN_POLICY)


class User(BaseModel):
    model_config = ConfigDict(extra="ignore")

    uid: str
    alias: str
    displayName: str
    email: str | None = None
    # Sign-in number only, never an SMS route (docs/CONTACT_REQ_DESIGN.md
    # decision 5).
    phone: str | None = None
    role: Role = "member"
    # docs/FAMILIES_DESIGN.md §1 decision 1: every user belongs to exactly
    # one family; `None` only for externals (decision 6) and for users
    # created before this field existed.
    familyId: str | None = None
    kind: Kind = "person"
    # docs/CONTACT_REQ_DESIGN.md decision 7: an external belongs to exactly
    # one family (`familyId` stays null so rules and `sameFam` are untouched).
    ownerFamilyId: str | None = None
    policy: Policy = Field(default_factory=lambda: Policy.model_validate(_DEFAULT_MEMBER_POLICY))
    notify: Notify = Field(default_factory=Notify)
    disabled: bool = False
    createdAt: datetime | None = None
    # docs/RELAY_SMS_DESIGN.md decision 1: the Twilio number (E.164) that
    # belongs to this person; `smsNumbers/{e164}` is its reverse index.
    smsNumber: str | None = None


class _Unset:
    """Sentinel type: "leave the field alone" (as opposed to `None`)."""

    def __repr__(self) -> str:
        return "UNSET"


UNSET = _Unset()


class SmsNumberTaken(Exception):
    """`smsNumbers/{e164}` is held by another uid (`holder_uid`)."""

    def __init__(self, holder_uid: str) -> None:
        super().__init__(f"sms number already assigned to {holder_uid!r}")
        self.holder_uid = holder_uid


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
    family_id: str | None = None,
    kind: Kind = "person",
    owner_family_id: str | None = None,
) -> User:
    """Creates `users/{uid}` and `aliases/{alias}` in one transaction --
    `aliases/{alias}` is created with `transaction.create`, which raises
    `AlreadyExists` (re-raised here as `AliasTaken`) if the alias is already
    taken, so the two documents can never be created out of sync.

    `family_id` defaults to `None` so every existing caller outside
    `app/routers/family.py` (task 1.3, which always passes the scope
    family) keeps working unchanged; `policy` is never a parameter -- it is
    always the role's default (docs/FAMILIES_DESIGN.md §2), computed here
    from `role`."""
    _validate_alias(alias)
    if kind == "person" and family_id is None:
        raise ValueError("a person must have a familyId")
    if kind == "external" and family_id is not None:
        raise ValueError("an external must not have a familyId")
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
                "familyId": family_id,
                "kind": kind,
                "ownerFamilyId": owner_family_id,
                "policy": _default_policy(role),
                "notify": {"alerts": True},
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
    # absence too).
    # An external (SMS contact) has no web client: its only backend is the
    # `sms` row `externals.get_or_create` adds.
    if kind == "person":
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
    notify_alerts: bool | None = None,
    sms_number: str | None | _Unset = UNSET,
) -> User:
    """Patch-semantics update of mutable fields. `alias` is intentionally
    not editable here -- changing it would orphan the old `aliases/{alias}`
    doc or require another transaction; not needed by any caller yet.

    `notify_alerts` backs `PATCH /api/me {notify: {alerts}}` (task 4.2) --
    `Notify` has exactly one field today, so overwriting the whole `notify`
    map is equivalent to a dotted-path update and needs no Firestore
    `FieldPath` machinery.
    """
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
    if notify_alerts is not None:
        updates["notify"] = {"alerts": notify_alerts}
    ref = get_db().collection("users").document(uid)
    if updates:
        try:
            ref.update(updates)
        except NotFound as exc:
            raise KeyError(f"no such user: {uid!r}") from exc
    if not isinstance(sms_number, _Unset):
        set_sms_number(uid, sms_number)
    fetched = get_user(uid)
    if fetched is None:
        raise KeyError(f"no such user: {uid!r}")
    return fetched


def _sms_numbers():
    return get_db().collection("smsNumbers")


def get_uid_for_sms_number(e164: str) -> str | None:
    """The uid `smsNumbers/{e164}` names, or `None`."""
    snap = _sms_numbers().document(e164).get()
    if not snap.exists:
        return None
    return (snap.to_dict() or {}).get("uid")


def _free_sms_index(e164: str, uid: str) -> None:
    ref = _sms_numbers().document(e164)
    snap = ref.get()
    if snap.exists and (snap.to_dict() or {}).get("uid") == uid:
        ref.delete()


def set_sms_number(uid: str, e164: str | None) -> User:
    """Assigns (or, with `None`, clears) `users/{uid}.smsNumber`. The index
    doc is reserved with `create()` *before* the user write; a number held by
    another uid raises `SmsNumberTaken(holder_uid)` (an index naming a user
    who no longer carries that number is a crash leftover and is taken
    over). The old number's index is freed only if it names this uid.
    Raises `KeyError` for no such user."""
    user = get_user(uid)
    if user is None:
        raise KeyError(f"no such user: {uid!r}")
    old = user.smsNumber
    # Known race, accepted at family volume: two admins assigning the same
    # number in the same second can both pass the reservation below.
    if e164 is not None:
        ref = _sms_numbers().document(e164)
        try:
            ref.create({"uid": uid})
        except AlreadyExists:
            holder = (ref.get().to_dict() or {}).get("uid")
            if holder != uid:
                holder_user = get_user(holder) if holder else None
                if holder_user is not None and holder_user.smsNumber == e164:
                    raise SmsNumberTaken(holder) from None
                ref.set({"uid": uid})
    get_db().collection("users").document(uid).update({"smsNumber": e164})
    if old is not None and old != e164:
        _free_sms_index(old, uid)
    fetched = get_user(uid)
    assert fetched is not None
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
        if user.smsNumber:
            _free_sms_index(user.smsNumber, uid)
    else:
        user_ref.delete()

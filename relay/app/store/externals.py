"""`users/{uid}` with `kind: 'external'` -- an SMS contact that is not a
Firebase Auth user (docs/CONTACT_REQ_DESIGN.md decision 7, superseding
docs/FAMILIES_DESIGN.md §1 decision 6's "one global external per number").

An external belongs to exactly one family: `familyId` is null (rules,
`sameFam` and the `locate` checks are unchanged), `ownerFamilyId` names the
family, `phone` is the E.164 number and `displayName` is that family's name
for it. Identity is `(fid, e164)`: `uid = "x_" + h[:16]`, `alias = "x" +
h[:11]` with `h = sha256(f"{fid}|{e164}")`, so `get_or_create` is idempotent
through `create_user`'s own transaction and lookups need no index. One `sms`
backend `{phone}` (verified + `adminVerified`) is written, and
`phoneIndex/{e164}.ext = {fid: uid}` (merge) is the reverse index the
shared-number Twilio webhook uses. `phoneIndex.uid/bid` stays persons-only.
"""

from __future__ import annotations

import hashlib

import phonenumbers

from app.db.firestore import get_db
from app.store import backends as backends_store
from app.store import users as users_store
from app.store.users import User

# docs/FAMILIES_TASKS.md 3.2's "Rules for this run": `phonenumbers`,
# default region "US" -- lets a bare 10-digit number (no country code) from
# the approved-numbers editor or the "New chat" phone box resolve the same
# way a US phone is normally typed, which `app/backends/sms_twilio.py`'s
# digit-strip `normalize_e164` cannot do (it has no notion of a default
# region, so a bare 10-digit US number comes out missing its `+1`).
DEFAULT_REGION = "US"


def normalize_phone(phone: str, *, default_region: str = DEFAULT_REGION) -> str:
    """E.164 for `phone`, parsed with `default_region` when `phone` carries
    no country code of its own. Deliberately checks `is_possible_number`
    (shape only: plausible digit count for the region) rather than
    `is_valid_number` (real assigned ranges) -- the fictional `+1555...`
    numbers this test suite (and any admin poking at the emulator) uses
    throughout are "possible" but not "valid", and this module has no
    business rejecting them. Raises `ValueError` (never `phonenumbers`'
    own exception type) on anything that isn't a plausible phone number at
    all, matching `sms_twilio.normalize_e164`'s own contract."""
    try:
        parsed = phonenumbers.parse(phone, default_region)
    except phonenumbers.NumberParseException as exc:
        raise ValueError(f"not a valid phone number: {phone!r}") from exc
    if not phonenumbers.is_possible_number(parsed):
        raise ValueError(f"not a valid phone number: {phone!r}")
    return phonenumbers.format_number(parsed, phonenumbers.PhoneNumberFormat.E164)


def contact_ids(family_id: str, e164: str) -> tuple[str, str]:
    """`(uid, alias)` of `family_id`'s SMS contact for `e164`."""
    h = hashlib.sha256(f"{family_id}|{e164}".encode()).hexdigest()
    return f"x_{h[:16]}", f"x{h[:11]}"


def get_family_contact(family_id: str, phone: str) -> User | None:
    """The family's SMS contact for `phone` (any spelling), or `None`."""
    uid, _alias = contact_ids(family_id, normalize_phone(phone))
    user = users_store.get_user(uid)
    if user is None or user.kind != "external":
        return None
    return user


def list_family_contacts(family_id: str) -> list[User]:
    return [
        u for u in users_store.list_users() if u.kind == "external" and u.ownerFamilyId == family_id
    ]


def get_or_create(family_id: str, phone: str, display_name: str) -> User:
    """Idempotent on `(family_id, phone)`; `display_name` is only used the
    first time. `phone` is normalized first, so equivalent spellings resolve
    to one contact. A different family holding the same number gets its own
    user."""
    e164 = normalize_phone(phone)
    uid, alias = contact_ids(family_id, e164)
    existing = users_store.get_user(uid)
    if existing is not None:
        return existing

    try:
        users_store.create_user(
            uid=uid,
            alias=alias,
            display_name=display_name,
            phone=e164,
            family_id=None,
            kind="external",
            owner_family_id=family_id,
        )
    except users_store.AliasTaken as exc:
        if users_store.get_uid_for_alias(alias) != uid:
            raise ValueError("contact alias collision") from exc
        # A concurrent create of the same contact won the race.
        raced = users_store.get_user(uid)
        if raced is None:
            raise ValueError("contact alias collision") from exc
        return raced
    backend = backends_store.create_backend(uid, kind="sms", config={"phone": e164})
    backends_store.update_backend(uid, backend.id, verified=True)
    # `adminVerified` -- not modelled on `Backend`; written directly, same
    # pattern `_create_admin_asserted_backend` uses.
    get_db().collection("users").document(uid).collection("backends").document(backend.id).update(
        {"adminVerified": True}
    )
    get_db().collection("phoneIndex").document(e164).set({"ext": {family_id: uid}}, merge=True)

    fetched = users_store.get_user(uid)
    assert fetched is not None
    return fetched

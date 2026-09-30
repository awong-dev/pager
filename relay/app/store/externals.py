"""`users/{uid}` with `kind: 'external'` -- docs/FAMILIES_DESIGN.md §1
decision 6, §4; docs/FAMILIES_TASKS.md 3.2.

An "external" is an SMS-only contact that is not a Firebase Auth user: no
sign-in, `familyId: null` (decision 1's "every user belongs to exactly one
family" explicitly excepts externals), alias = the E.164 number's digits
(no leading `+`, fits `app.store.users.ALIAS_RE`), one `sms` backend
`{config.phone}` marked `adminVerified` (same shape
`app/routers/admin.py`'s `_create_admin_asserted_backend` writes for a
linked pager contact), and a `phoneIndex/{e164}` entry so inbound Twilio
webhooks and `backends_store.get_by_phone` resolve it exactly like any
other verified sms backend.

Externals are **global**, one per number (docs/FAMILIES_DESIGN.md §1
decision 6's "inbound SMS must resolve the sender before it knows the
family") -- `get_or_create` is idempotent on the number via `phoneIndex`,
not per-family, so two families approving the same number both get the
same `uid`.
"""

from __future__ import annotations

import secrets

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


def alias_for_phone(e164: str) -> str:
    """`users.alias` for an external: the E.164 digits, no leading `+` --
    docs/FAMILIES_DESIGN.md §1 decision 6."""
    return e164.lstrip("+")


def _mint_uid() -> str:
    # docs/FAMILIES_TASKS.md 3.2's "Rules for this run": "uid minted (`x_`
    # + random)" -- distinguishes an external's uid from a Firebase Auth
    # uid at a glance (externals have none) without needing a `kind` lookup
    # everywhere a uid alone is logged.
    return f"x_{secrets.token_hex(8)}"


def get_or_create(phone: str, display_name: str) -> User:
    """Idempotent on `phone` (via `phoneIndex`, checked before creating
    anything): a second call with the same number, from any family, returns
    the same `users/{uid}` -- `display_name` is only used the first time.
    `phone` is normalized (see `normalize_phone`) before either the lookup
    or the create, so equivalent spellings of the same number
    (`+1 555 123 4567` vs `15551234567`) resolve to one external."""
    e164 = normalize_phone(phone)
    found = backends_store.get_by_phone(e164)
    if found is not None:
        uid, _bid = found
        existing = users_store.get_user(uid)
        if existing is not None:
            return existing

    uid = _mint_uid()
    users_store.create_user(
        uid=uid,
        alias=alias_for_phone(e164),
        display_name=display_name,
        family_id=None,
        kind="external",
    )
    backend = backends_store.create_backend(uid, kind="sms", config={"phone": e164})
    backends_store.update_backend(uid, backend.id, verified=True)
    backends_store.set_phone_index(e164, uid, backend.id)
    # `adminVerified` -- not modelled on `Backend` (`app/store/backends.py`
    # is outside this task's `Files` list), same "write it directly, it's
    # dropped on read by `extra='ignore'` until a task that owns that
    # module adds the field" pattern `_create_admin_asserted_backend`
    # already uses.
    get_db().collection("users").document(uid).collection("backends").document(backend.id).update(
        {"adminVerified": True}
    )

    fetched = users_store.get_user(uid)
    assert fetched is not None
    return fetched

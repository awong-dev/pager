"""Direct unit tests of `app.store.users` against the Firestore emulator:
`create_user`'s `aliases/{alias}` uniqueness invariant (docs/SERVER_PLAN.md
§3, `app/store/users.py`'s module docstring) -- the alias and the
`users/{uid}` doc it points at are created in one transaction, so a losing
`create_user` must leave no orphan document behind."""

from __future__ import annotations

import pytest

from app.db.firestore import get_db
from app.store import families as families_store
from app.store import users as users_store
from app.store.users import AliasTaken


def test_create_user_duplicate_alias_raises_and_leaves_no_orphan():
    first = users_store.create_user(
        uid="uid-first",
        alias="dupealias",
        display_name="First User",
    )
    assert first.alias == "dupealias"

    with pytest.raises(AliasTaken):
        users_store.create_user(
            uid="uid-second",
            alias="dupealias",
            display_name="Second User",
        )

    # The alias still points at the first uid...
    assert users_store.get_uid_for_alias("dupealias") == "uid-first"

    # ...and the losing transaction left no orphan `users/{uid}` document
    # for the second attempt's uid.
    assert users_store.get_user("uid-second") is None
    assert get_db().collection("users").document("uid-second").get().exists is False


def test_create_user_member_defaults_to_family_id_and_people_policy():
    """docs/FAMILIES_DESIGN.md §2: `member` defaults to `people`/`people`."""
    family = families_store.create_family(name="Members Family", created_by="root-uid")
    user = users_store.create_user(
        uid="member-uid",
        alias="memberone",
        display_name="Member One",
        role="member",
        family_id=family.id,
    )
    assert user.familyId == family.id
    assert user.role == "member"
    assert user.kind == "person"
    assert user.policy.out == "people"
    assert user.policy.in_ == "people"
    assert user.notify.alerts is True


def test_create_user_admin_defaults_to_open_any_policy():
    """docs/FAMILIES_DESIGN.md §2: `admin`/`super` default to `open`/`any`."""
    user = users_store.create_user(
        uid="admin-policy-uid",
        alias="adminpolicy",
        display_name="Admin Policy",
        role="admin",
    )
    assert user.policy.out == "open"
    assert user.policy.in_ == "any"

    superuser = users_store.create_user(
        uid="super-policy-uid",
        alias="superpolicy",
        display_name="Super Policy",
        role="super",
    )
    assert superuser.policy.out == "open"
    assert superuser.policy.in_ == "any"


def test_create_user_person_without_family_is_refused():
    """docs/CONTACT_REQ_DESIGN.md decision 3. (The unwrapped function: the
    suite's conftest shim gives bare fixture users a family.)"""
    strict = users_store.create_user.__wrapped__
    with pytest.raises(ValueError, match="a person must have a familyId"):
        strict(uid="no-family-uid", alias="nofamily", display_name="No Family", kind="person")
    assert users_store.get_user("no-family-uid") is None
    assert users_store.get_uid_for_alias("nofamily") is None


def test_create_user_external_with_family_id_is_refused():
    strict = users_store.create_user.__wrapped__
    with pytest.raises(ValueError):
        strict(
            uid="x_bad",
            alias="xbad",
            display_name="x",
            kind="external",
            family_id="fam1",
            owner_family_id="fam1",
        )


def test_create_user_external_kind():
    user = users_store.create_user(
        uid="external-uid",
        alias="15551234567",
        display_name="+1 555 123 4567",
        role="member",
        kind="external",
    )
    assert user.kind == "external"
    assert user.familyId is None

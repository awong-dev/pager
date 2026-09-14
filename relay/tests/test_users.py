"""Direct unit tests of `app.store.users` against the Firestore emulator:
`create_user`'s `aliases/{alias}` uniqueness invariant (docs/SERVER_PLAN.md
§3, `app/store/users.py`'s module docstring) -- the alias and the
`users/{uid}` doc it points at are created in one transaction, so a losing
`create_user` must leave no orphan document behind."""

from __future__ import annotations

import pytest

from app.db.firestore import get_db
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

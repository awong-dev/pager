"""`python -m app.bootstrap` -- docs/SERVER_PLAN.md §5.3: creates the first
admin (Firebase Auth user + `users/{uid}` doc with `role: 'admin'` + the
`admin` custom claim), idempotently."""

from __future__ import annotations

from firebase_admin import auth as fb_auth

from app.bootstrap import bootstrap_admin
from app.store import users as users_store


def test_bootstrap_creates_admin_user_and_claim():
    uid = bootstrap_admin("root@example.com")

    auth_user = fb_auth.get_user(uid)
    assert auth_user.email == "root@example.com"
    assert auth_user.custom_claims and auth_user.custom_claims.get("admin") is True

    user = users_store.get_user(uid)
    assert user is not None
    assert user.role == "admin"
    assert user.alias == "admin"


def test_bootstrap_is_idempotent():
    uid1 = bootstrap_admin("root2@example.com")
    uid2 = bootstrap_admin("root2@example.com")
    assert uid1 == uid2

    users = users_store.list_users()
    assert sum(1 for u in users if u.uid == uid1) == 1


def test_bootstrap_custom_alias_and_display_name():
    uid = bootstrap_admin("root3@example.com", alias="superadmin", display_name="Super Admin")
    user = users_store.get_user(uid)
    assert user is not None
    assert user.alias == "superadmin"
    assert user.displayName == "Super Admin"

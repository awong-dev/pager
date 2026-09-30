"""`python -m app.bootstrap` -- docs/FAMILIES_DESIGN.md §7: on an empty
database, creates `families/default`, the bootstrap user as `role: 'super'`
in that family, `{role, fam}` claims, and `settings/meta.schemaVersion = 2`.
Idempotent."""

from __future__ import annotations

from firebase_admin import auth as fb_auth

from app.bootstrap import bootstrap_admin
from app.store import families as families_store
from app.store import settings as settings_store
from app.store import users as users_store


def test_bootstrap_creates_default_family():
    bootstrap_admin("root@example.com")

    family = families_store.get_family("default")
    assert family is not None
    assert family.name == "Home"


def test_bootstrap_creates_super_user_in_default_family():
    uid = bootstrap_admin("root@example.com")

    user = users_store.get_user(uid)
    assert user is not None
    assert user.role == "super"
    assert user.familyId == "default"
    assert user.kind == "person"
    assert user.alias == "admin"
    assert user.policy.out == "open"
    assert user.policy.in_ == "any"


def test_bootstrap_sets_role_and_fam_claims_only():
    uid = bootstrap_admin("root@example.com")

    auth_user = fb_auth.get_user(uid)
    assert auth_user.custom_claims == {"role": "super", "fam": "default"}


def test_bootstrap_sets_schema_version_2():
    bootstrap_admin("root@example.com")

    meta = settings_store.get_meta()
    assert meta.schemaVersion == 2


def test_bootstrap_custom_family_name():
    bootstrap_admin("root@example.com", family_name="The Wongs")

    family = families_store.get_family("default")
    assert family is not None
    assert family.name == "The Wongs"


def test_bootstrap_is_idempotent():
    uid1 = bootstrap_admin("root2@example.com")
    uid2 = bootstrap_admin("root2@example.com")
    assert uid1 == uid2

    users = users_store.list_users()
    assert sum(1 for u in users if u.uid == uid1) == 1

    families = families_store.list_families()
    assert sum(1 for f in families if f.id == "default") == 1


def test_bootstrap_rerun_does_not_change_existing_family_name():
    bootstrap_admin("root3@example.com", family_name="First Name")
    bootstrap_admin("root3@example.com", family_name="Second Name")

    family = families_store.get_family("default")
    assert family is not None
    assert family.name == "First Name"


def test_bootstrap_custom_alias_and_display_name():
    uid = bootstrap_admin("root4@example.com", alias="superadmin", display_name="Super Admin")
    user = users_store.get_user(uid)
    assert user is not None
    assert user.alias == "superadmin"
    assert user.displayName == "Super Admin"

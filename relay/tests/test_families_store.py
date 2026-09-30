"""Direct unit tests of `app.store.families` against the Firestore emulator
-- docs/FAMILIES_DESIGN.md §3."""

from __future__ import annotations

import pytest

from app.store import families as families_store


def test_create_and_get_family_assigns_id_when_not_given():
    family = families_store.create_family(name="Home", created_by="root-uid")
    assert family.id
    assert family.name == "Home"
    assert family.smsNumber is None
    assert family.blockedNumbers == []
    assert family.createdBy == "root-uid"

    fetched = families_store.get_family(family.id)
    assert fetched == family


def test_create_family_with_pinned_id_is_idempotent_key():
    family = families_store.create_family(
        name="Home", created_by="root-uid", family_id="default"
    )
    assert family.id == "default"


def test_get_family_missing_returns_none():
    assert families_store.get_family("no-such-family") is None


def test_list_families():
    families_store.create_family(name="List A", created_by="root-uid")
    families_store.create_family(name="List B", created_by="root-uid")
    names = {f.name for f in families_store.list_families()}
    assert {"List A", "List B"} <= names


def test_update_family_name_and_sms_number():
    family = families_store.create_family(name="Old Name", created_by="root-uid")
    updated = families_store.update_family(family.id, name="New Name", sms_number="+15550001111")
    assert updated.name == "New Name"
    assert updated.smsNumber == "+15550001111"


def test_update_family_missing_raises():
    with pytest.raises(KeyError):
        families_store.update_family("no-such-family", name="X")

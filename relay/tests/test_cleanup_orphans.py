"""`scripts/cleanup_orphan_persons.py` -- docs/CONTACT_REQ_DESIGN.md decision 3."""

from __future__ import annotations

import pytest
from firebase_admin import auth as fb_auth

from app.db.firestore import get_db
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import devices as devices_store
from app.store import families as families_store
from app.store import users as users_store
from scripts import cleanup_orphan_persons as script
from tests.firebase_test_utils import seed_legacy_contact_request

PHONE = "+12065550142"


def _bv(device_id: str) -> int:
    raw = get_db().collection("devices").document(device_id).get().to_dict() or {}
    return int(raw.get("bookVersion", 0))


def _seed() -> tuple[str, str]:
    """A family with a kid + pager, and an orphan person made the old way
    (Auth user, family-less user with a phone, mutual edges, an approved
    request and its alert). Returns `(familyId, orphanUid)`."""
    family = families_store.create_family(name="F", created_by="root")
    users_store.create_user(uid="kid", alias="kid", display_name="Kid", family_id=family.id)
    devices_store.create_device(
        device_id="pgr-o1",
        owner_uid="kid",
        label="d",
        mqtt_username="pgr-o1",
        mqtt_password_hash="x",
        auth_mode="password",
    )
    auth_user = fb_auth.create_user(email="aphone1@example.com")
    uid = auth_user.uid
    users_store.create_user(uid=uid, alias="aphone1", display_name="Aunt", family_id=family.id)
    get_db().collection("users").document(uid).update({"familyId": None})  # the orphan state
    users_store.update_user(uid, phone=PHONE)
    get_db().collection("users").document(uid).collection("book").document("kid").set({"nick": "x"})
    allow_store.set_edge("kid", uid, message=True, locate=False)
    allow_store.set_edge(uid, "kid", message=True, locate=False)

    seed_legacy_contact_request(
        "pgr-o1", "kid", "u_o1", "Aunt", PHONE, status="approved", family_id=family.id
    )
    # An unrelated pending request and its alert must survive.
    seed_legacy_contact_request(
        "pgr-o1", "kid", "u_o2", "Other", "+12065550999", family_id=family.id
    )
    return family.id, uid


def _snapshot() -> dict[str, int]:
    db = get_db()
    return {
        "users": len(list(db.collection("users").stream())),
        "aliases": len(list(db.collection("aliases").stream())),
        "allow": len(list(db.collection("allow").stream())),
        "requests": len(list(db.collection("contactRequests").stream())),
    }


def test_dry_run_changes_nothing_and_lists_the_orphan(capsys: pytest.CaptureFixture[str]):
    fid, uid = _seed()
    before = _snapshot()
    bv = _bv("pgr-o1")
    alert_count = len(alerts_store.list_alerts(fid, "all"))

    assert script.main([]) == 0
    out = capsys.readouterr().out
    assert uid in out and "@aphone1" in out and "dry run" in out
    assert "1 family-less" in out  # the kid is not an orphan

    assert _snapshot() == before
    assert _bv("pgr-o1") == bv
    assert len(alerts_store.list_alerts(fid, "all")) == alert_count
    assert fb_auth.get_user(uid) is not None


def test_apply_without_uid_is_a_usage_error():
    with pytest.raises(SystemExit):
        script.main(["--apply"])


def test_apply_deletes_the_orphan_and_its_traces(capsys: pytest.CaptureFixture[str]):
    fid, uid = _seed()
    # A message that must stay as history.
    msg_count_before = len(list(get_db().collection("messages").stream()))
    bv = _bv("pgr-o1")

    assert script.main(["--apply", "--uid", uid]) == 0
    out = capsys.readouterr().out
    assert "nudge pending; the next /status republishes" in out

    assert users_store.get_user(uid) is None
    assert users_store.get_uid_for_alias("aphone1") is None
    with pytest.raises(fb_auth.UserNotFoundError):
        fb_auth.get_user(uid)
    assert allow_store.get_edge("kid", uid) is None
    assert allow_store.get_edge(uid, "kid") is None
    assert backends_store.list_backends(uid) == []
    assert list(get_db().collection("users").document(uid).collection("book").stream()) == []
    # The approved request and its alert are gone; the unrelated ones stay.
    assert contacts_store.get_by_device_and_req("pgr-o1", "u_o1") is None
    assert contacts_store.get_by_device_and_req("pgr-o1", "u_o2") is not None
    remaining = alerts_store.list_alerts(fid, "all")
    assert [a.contactRequestKey for a in remaining] == [contacts_store.key("pgr-o1", "u_o2")]
    # The kid, who had an edge to the orphan, is nudged; messages untouched.
    assert users_store.get_user("kid") is not None
    assert _bv("pgr-o1") == bv + 1
    assert len(list(get_db().collection("messages").stream())) == msg_count_before


def test_apply_refuses_a_non_orphan_and_changes_nothing(capsys: pytest.CaptureFixture[str]):
    _fid, uid = _seed()
    before = _snapshot()

    assert script.main(["--apply", "--uid", uid, "--uid", "kid"]) == 2
    assert "refusing kid" in capsys.readouterr().out
    assert _snapshot() == before
    assert users_store.get_user(uid) is not None
    assert users_store.get_user("kid") is not None

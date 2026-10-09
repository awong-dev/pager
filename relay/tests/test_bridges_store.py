"""`app.store.bridges` + `app.bridgeauth` -- docs/BRIDGE_PHONE_DESIGN.md decisions 1-3."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

from app import bridgeauth
from app.store import bridges as bridges_store


def test_create_get_list():
    b = bridges_store.create("kid", "fam1", "Kitchen phone", "adm")
    assert b.id.startswith("b_") and len(b.id) == 10
    assert not b.paired and b.tokenHash is None
    assert b.status.tier2Count == 0 and b.caps.sms is False
    assert bridges_store.get(b.id) == b
    assert bridges_store.get("b_nope") is None
    other = bridges_store.create("kid2", "fam2", "x", "adm")
    assert [x.id for x in bridges_store.list_for_family("fam1")] == [b.id]
    assert {x.id for x in bridges_store.list_all()} == {b.id, other.id}
    assert [x.id for x in bridges_store.list_for_owner("kid2")] == [other.id]


def test_get_by_sms_number_matches_either_field_and_needs_a_token():
    b = bridges_store.create("kid", "fam1", "p", "adm")
    bridges_store.set_numbers(b.id, sim_number="+12065550100", voice_number="+12065550199")
    assert bridges_store.get_by_sms_number("+12065550100") is None  # unpaired
    _, token_hash = bridgeauth.mint_token(b.id)
    bridges_store.set_token_hash(b.id, token_hash, caps=bridges_store.BridgeCaps(), status={})
    assert bridges_store.get_by_sms_number("+12065550100").id == b.id
    assert bridges_store.get_by_sms_number("+12065550199").id == b.id
    assert bridges_store.get_by_sms_number("+12065550000") is None
    bridges_store.unpair(b.id)
    assert bridges_store.get_by_sms_number("+12065550100") is None
    after = bridges_store.get(b.id)
    assert after.tokenHash is None and after.fcmToken is None and after.status.unpaired


def test_touch_merges_status_and_keeps_relay_owned_fields():
    b = bridges_store.create("kid", "fam1", "p", "adm")
    bridges_store.set_error(b.id, "oops")
    bridges_store.increment_tier2(b.id)
    bridges_store.touch(
        b.id, {"battery": 80, "listenerBound": True, "tier2Count": 99, "error": None}, "fcm-1"
    )
    got = bridges_store.get(b.id)
    assert got.status.battery == 80 and got.status.listenerBound is True
    assert got.status.tier2Count == 1 and got.status.error == "oops"
    assert got.fcmToken == "fcm-1" and got.lastSeenAt is not None


def test_setters():
    b = bridges_store.create("kid", "fam1", "p", "adm")
    bridges_store.set_label(b.id, "New")
    bridges_store.set_owner(b.id, "kid2")
    got = bridges_store.get(b.id)
    assert got.label == "New" and got.ownerUid == "kid2"


def test_pair_code_is_single_use_and_expires():
    b = bridges_store.create("kid", "fam1", "p", "adm")
    code, expires = bridges_store.create_pair_code(b.id)
    assert len(code) == 8 and code.isdigit()
    assert expires > datetime.now(UTC) + timedelta(minutes=9)
    assert bridges_store.consume_pair_code(code) == b.id
    assert bridges_store.consume_pair_code(code) is None

    old, _ = bridges_store.create_pair_code(b.id, now=datetime.now(UTC) - timedelta(minutes=11))
    assert bridges_store.list_expired_pair_codes(datetime.now(UTC)) == [old]
    assert bridges_store.consume_pair_code(old) is None
    assert bridges_store.list_expired_pair_codes(datetime.now(UTC)) == []


def test_verify_token():
    b = bridges_store.create("kid", "fam1", "p", "adm")
    token, token_hash = bridgeauth.mint_token(b.id)
    assert bridgeauth.verify(token) is None  # no hash stored yet
    bridges_store.set_token_hash(b.id, token_hash, caps=bridges_store.BridgeCaps(), status={})
    assert bridgeauth.verify(token).id == b.id
    assert bridgeauth.verify(token + "x") is None
    assert bridgeauth.verify(f"{b.id}.other") is None
    assert bridgeauth.verify("garbage") is None
    assert bridgeauth.verify(".x") is None

"""Cross-family bridge fixes (docs/BRIDGE_PHONE_DESIGN.md, cross-family review
9 Oct 2026): relay-wide number uniqueness at pair, owner-preferring lookup,
transactional channel records."""

from __future__ import annotations

from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import bridge_numbers
from app.db.firestore import get_db
from app.routing import Routing
from app.store import backends as backends_store
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import users as users_store
from tests.bridge_world import (  # noqa: F401
    MOM,
    SIM,
    VOICE,
    World,
    broker,
    client,
    fcm,
    pair_bridge,
    world,
)
from tests.fake_transport import FakeBrokerClient

OTHER = "+12065550177"


def _family_b() -> World:
    family = families_store.create_family(name="G", created_by="root")
    fb_auth.create_user(uid="adm2", email="adm2@example.com")
    users_store.create_user(
        uid="adm2", alias="adm2", display_name="adm2", role="admin", family_id=family.id
    )
    users_store.create_user(uid="kid2", alias="kid2", display_name="Kid2", family_id=family.id)
    return World(family_id=family.id, admin_headers={})


def _code_b(b: World) -> tuple[str, str]:
    bridge = bridges_store.create("kid2", b.family_id, "phone", "adm2")
    return bridge.id, bridges_store.create_pair_code(bridge.id)[0]


def _pair(client: TestClient, code: str, **numbers: str):
    return client.post("/bridge/pair", json={"code": code, "caps": {"sms": True}, **numbers})


def test_pair_voice_number_of_another_family_is_409_and_keeps_the_code(
    client: TestClient, world: World
):
    pair_bridge(client, world, sim=None, voice=VOICE)
    b = _family_b()
    bid, code = _code_b(b)
    resp = _pair(client, code, voiceNumber=VOICE)
    assert resp.status_code == 409
    assert resp.json()["detail"] in (
        "that number is used by another bridge phone",
        "that number belongs to @kid",
    )
    assert not bridges_store.get(bid).paired
    assert _pair(client, code, voiceNumber=OTHER).status_code == 200
    assert bridges_store.get(bid).paired


def test_pair_sim_number_of_another_family_is_409_and_keeps_the_code(
    client: TestClient, world: World
):
    pair_bridge(client, world, sim=SIM)
    b = _family_b()
    _, code = _code_b(b)
    resp = _pair(client, code, simNumber=SIM)
    assert resp.status_code == 409
    assert _pair(client, code, simNumber=OTHER).status_code == 200
    assert users_store.get_user("kid2").smsNumber == OTHER


def test_pair_succeeds_after_the_first_bridge_is_unpaired(client: TestClient, world: World, broker):
    first, _ = pair_bridge(client, world, sim=None, voice=VOICE)
    bridges_store.unpair(first.id)
    bridge_numbers.release_numbers(bridges_store.get(first.id), broker)
    b = _family_b()
    _, code = _code_b(b)
    assert _pair(client, code, voiceNumber=VOICE).status_code == 200
    assert users_store.get_user("kid2").smsNumber == VOICE


def test_unknown_code_is_404_before_any_number_check(client: TestClient, world: World):
    pair_bridge(client, world, sim=None, voice=VOICE)
    assert _pair(client, "00000000", voiceNumber=VOICE).status_code == 404


def test_peek_pair_code_does_not_consume(client: TestClient, world: World):
    bridge = bridges_store.create("kid", world.family_id, "phone", "adm")
    code, _ = bridges_store.create_pair_code(bridge.id)
    assert bridges_store.peek_pair_code(code) == bridge.id
    assert bridges_store.peek_pair_code(code) == bridge.id
    assert bridges_store.peek_pair_code("00000000") is None
    assert bridges_store.consume_pair_code(code) == bridge.id
    assert bridges_store.peek_pair_code(code) is None


def _seed_duplicate(a: World, b: World) -> tuple[bridges_store.Bridge, bridges_store.Bridge]:
    """Two paired bridges in different families sharing one Voice number,
    written straight to the store (pre-fix data)."""
    out = []
    for world_, owner, admin in ((a, "kid", "adm"), (b, "kid2", "adm2")):
        br = bridges_store.create(owner, world_.family_id, "phone", admin)
        bridges_store.set_token_hash(
            br.id, "h", caps=bridge_numbers.caps_for(False, False, None, VOICE, whatsapp=False),
            status={},
        )
        bridges_store.set_numbers(
            br.id, sim_number=None, voice_number=VOICE,
            caps=bridge_numbers.caps_for(False, False, None, VOICE, whatsapp=False),
        )
        out.append(bridges_store.get(br.id))
    return out[0], out[1]


def test_lookup_prefers_the_owners_own_bridge(client: TestClient, world: World):
    b = _family_b()
    br_a, br_b = _seed_duplicate(world, b)
    assert bridges_store.get_by_sms_number(VOICE, prefer_owner="kid2").id == br_b.id
    assert bridges_store.get_by_sms_number(VOICE, prefer_owner="kid").id == br_a.id
    assert bridges_store.get_by_sms_number(VOICE, prefer_owner="nobody") is not None
    assert bridges_store.get_by_sms_number(VOICE) is not None


def test_sms_from_family_a_lands_in_family_a_outbox(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    b = _family_b()
    # B's bridge first so an unpreferred lookup could pick it.
    br_b = bridges_store.create("kid2", b.family_id, "phone", "adm2")
    caps = bridge_numbers.caps_for(False, False, None, VOICE, whatsapp=False)
    bridges_store.set_token_hash(br_b.id, "h", caps=caps, status={})
    bridges_store.set_numbers(br_b.id, sim_number=None, voice_number=VOICE, caps=caps)
    br_a = bridges_store.create("kid", world.family_id, "phone", "adm")
    bridges_store.set_token_hash(br_a.id, "h", caps=caps, status={})
    bridges_store.set_numbers(br_a.id, sim_number=None, voice_number=VOICE, caps=caps)
    users_store.set_sms_number("kid", VOICE)

    from app.store import allow as allow_store

    get_db().collection("users").document("kid").update(
        {"policy": {"out": "people_sms", "in": "people_sms"}}
    )
    ext = externals_store.get_or_create(world.family_id, MOM, "Mom")
    allow_store.set_edge("kid", ext.uid, message=True, locate=False)
    result = Routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="hi",
        origin_backend_kind="pager", wire_id=None,
    )
    assert result.rejected == [], result.rejected
    assert len(bridge_outbox.list_pending(br_a.id)) == 1
    assert bridge_outbox.list_pending(br_b.id) == []


# ---------------------------------------------------------------------------
# record_channel is transactional
# ---------------------------------------------------------------------------


def _ext_row(world: World):
    ext = externals_store.get_or_create(world.family_id, MOM, "Mom")
    bid = externals_store.ensure_sms_backend(ext)
    return ext, bid


def test_two_members_channels_both_survive(client: TestClient, world: World):
    ext, bid = _ext_row(world)
    backends_store.record_member_channel(ext.uid, bid, "kid", "gvoice", "t1")
    backends_store.record_member_channel(ext.uid, bid, "adm", "whatsapp", "jid@x")
    cfg = backends_store.get_backend(ext.uid, bid).config
    assert cfg["via"] == {"kid": "gvoice", "adm": "whatsapp"}
    assert cfg["voiceConv"] == {"kid": "t1", "adm": "jid@x"}


def test_concurrent_members_channels_both_survive(client: TestClient, world: World):
    from concurrent.futures import ThreadPoolExecutor

    ext, bid = _ext_row(world)
    members = [f"m{i}" for i in range(6)]
    with ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(
            lambda m: backends_store.record_member_channel(ext.uid, bid, m, "gvoice", f"c-{m}"),
            members,
        ))
    cfg = backends_store.get_backend(ext.uid, bid).config
    assert set(cfg["via"]) == set(members) and set(cfg["voiceConv"]) == set(members)


def test_unchanged_channel_writes_nothing(client: TestClient, world: World, monkeypatch):
    ext, bid = _ext_row(world)
    backends_store.record_member_channel(ext.uid, bid, "kid", "sms", None)
    calls: list[object] = []
    from google.cloud.firestore import Transaction

    real = Transaction.update
    monkeypatch.setattr(Transaction, "update", lambda self, *a, **k: calls.append(a) or real(self, *a, **k))
    backends_store.record_member_channel(ext.uid, bid, "kid", "sms", None)
    assert calls == []
    backends_store.record_member_channel(ext.uid, bid, "kid", "gvoice", "t")
    assert len(calls) == 1


def test_missing_row_is_a_noop(client: TestClient, world: World):
    ext, _ = _ext_row(world)
    backends_store.record_member_channel(ext.uid, "nope", "kid", "sms", None)
    assert backends_store.get_backend(ext.uid, "nope") is None

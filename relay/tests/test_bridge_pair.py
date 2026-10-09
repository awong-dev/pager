"""`POST /bridge/pair`, `POST /bridge/heartbeat` -- docs/BRIDGE_PHONE_DESIGN.md
decisions 2, 3, O1 (revised)."""

from __future__ import annotations

from datetime import UTC, datetime

from fastapi.testclient import TestClient

from app import jobs
from app.store import bridges as bridges_store
from app.store import users as users_store
from tests.bridge_world import (  # noqa: F401  (fixtures)
    SIM,
    VOICE,
    World,
    broker,
    client,
    fcm,
    make_pager_device,
    pair_bridge,
    world,
)
from tests.fake_transport import FakeBrokerClient


def _new_code(world: World, owner: str = "kid") -> tuple[str, str]:
    b = bridges_store.create(owner, world.family_id, "phone", "adm")
    return b.id, bridges_store.create_pair_code(b.id)[0]


def test_pair_happy_path_sets_owner_number(client: TestClient, world: World):
    bridge, headers = pair_bridge(client, world)
    assert bridge.paired and bridge.simNumber == SIM and bridge.voiceNumber is None
    assert bridge.caps.sms and not bridge.caps.gvoice
    assert bridge.status.accounts == ["kid@example.com"] and bridge.status.version == "1.0"
    assert users_store.get_user("kid").smsNumber == SIM
    assert users_store.get_uid_for_sms_number(SIM) == "kid"
    assert client.post("/bridge/heartbeat", json={"status": {}}, headers=headers).status_code == 200


def test_pair_pushes_a_book_bump(client: TestClient, world: World, broker: FakeBrokerClient):
    make_pager_device("pgr-b1", "kid")
    before = len(broker.published)
    pair_bridge(client, world)
    assert len(broker.published) > before


def test_reused_code_is_404(client: TestClient, world: World):
    _, code = _new_code(world)
    body = {"code": code, "accounts": [], "caps": {}}
    assert client.post("/bridge/pair", json=body).status_code == 200
    assert client.post("/bridge/pair", json=body).status_code == 404
    assert client.post("/bridge/pair", json={"code": "00000000"}).status_code == 404


def test_pair_422_on_garbage_number(client: TestClient, world: World):
    _, code = _new_code(world)
    resp = client.post("/bridge/pair", json={"code": code, "simNumber": "abc"})
    assert resp.status_code == 422
    resp = client.post("/bridge/pair", json={"code": code, "voiceNumber": "12"})
    assert resp.status_code == 422
    # The code was not burned by the rejected bodies.
    assert client.post("/bridge/pair", json={"code": code}).status_code == 200


def test_wrong_and_missing_tokens_are_401(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world)
    hb = {"status": {}}
    assert client.post("/bridge/heartbeat", json=hb).status_code == 401
    bad = {"Authorization": f"Bearer {bridge.id}.nope"}
    r = client.post("/bridge/heartbeat", json=hb, headers=bad)
    assert r.status_code == 401 and r.json() == {"detail": "unauthorized"}
    basic = {"Authorization": "Basic x"}
    assert client.post("/bridge/heartbeat", json=hb, headers=basic).status_code == 401
    assert client.post("/bridge/heartbeat", json=hb, headers={"Authorization": "Bearer"}).status_code == 401


def test_token_of_an_unpaired_bridge_is_401(client: TestClient, world: World):
    bridge, headers = pair_bridge(client, world)
    bridges_store.unpair(bridge.id)
    assert client.post("/bridge/heartbeat", json={"status": {}}, headers=headers).status_code == 401


def test_heartbeat_updates_status(client: TestClient, world: World):
    bridge, headers = pair_bridge(client, world)
    resp = client.post(
        "/bridge/heartbeat",
        json={
            "status": {"battery": 55, "listenerBound": True, "simNumber": SIM, "smsDefault": True},
            "fcmToken": "tok-1",
        },
        headers=headers,
    )
    assert resp.status_code == 200 and resp.json() == {"pending": 0}
    got = bridges_store.get(bridge.id)
    assert got.status.battery == 55 and got.status.listenerBound and got.status.smsDefault
    assert got.fcmToken == "tok-1" and got.lastSeenAt is not None and got.status.error is None


def test_heartbeat_with_a_different_sim_flags_error_and_leaves_sms_number(
    client: TestClient, world: World
):
    bridge, headers = pair_bridge(client, world)
    client.post(
        "/bridge/heartbeat", json={"status": {"simNumber": "+12065550777"}}, headers=headers
    )
    got = bridges_store.get(bridge.id)
    assert got.status.error == "SIM changed" and got.simNumber == SIM
    assert users_store.get_user("kid").smsNumber == SIM
    assert users_store.get_uid_for_sms_number("+12065550777") is None
    # Back to the accepted SIM clears the flag.
    client.post("/bridge/heartbeat", json={"status": {"simNumber": SIM}}, headers=headers)
    assert bridges_store.get(bridge.id).status.error is None


def test_sms_number_taken_is_recorded_and_user_untouched(client: TestClient, world: World):
    users_store.create_user(uid="sis", alias="sis", display_name="Sis", family_id=world.family_id)
    users_store.set_sms_number("sis", SIM)
    bridge, _ = pair_bridge(client, world)
    assert "@sis" in bridges_store.get(bridge.id).status.error
    assert users_store.get_user("kid").smsNumber is None
    assert users_store.get_user("sis").smsNumber == SIM


def test_voice_only_pair_uses_the_voice_number(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world, sim=None, voice=VOICE)
    assert bridge.simNumber is None and bridge.voiceNumber == VOICE
    assert bridge.caps.gvoice and not bridge.caps.sms
    assert users_store.get_user("kid").smsNumber == VOICE
    assert bridges_store.get_by_sms_number(VOICE).id == bridge.id


def test_sim_wins_over_voice_when_both_present(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world, sim=SIM, voice=VOICE)
    assert users_store.get_user("kid").smsNumber == SIM
    assert bridge.caps.sms and bridge.caps.gvoice


def test_pair_rate_limit_is_global_across_forwarded_ips(client: TestClient, world: World):
    codes = [_new_code(world)[1] for _ in range(11)]
    statuses = []
    for i, code in enumerate(codes):
        r = client.post(
            "/bridge/pair", json={"code": code}, headers={"X-Forwarded-For": f"10.0.0.{i}"}
        )
        statuses.append(r.status_code)
    assert statuses[:10] == [200] * 10
    assert statuses[10] == 429


def test_sweep_deletes_expired_pair_codes():
    b = bridges_store.create("kid", "fam1", "p", "adm")
    bridges_store.create_pair_code(b.id, now=datetime(2020, 1, 1, tzinfo=UTC))
    live, _ = bridges_store.create_pair_code(b.id)
    result = jobs.sweep()
    assert result.bridgePairCodesDeleted == 1
    assert bridges_store.consume_pair_code(live) == b.id

"""`/api/family/bridges*` -- docs/BRIDGE_PHONE_DESIGN.md decisions 1-3, O1 (revised)."""

from __future__ import annotations

import json

from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.db.firestore import get_db
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import families as families_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.bridge_world import (  # noqa: F401
    MOM,
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
from tests.firebase_test_utils import auth_header


def _create(client: TestClient, world: World, owner: str = "kid", label: str = "Kitchen"):
    r = client.post(
        "/api/family/bridges", json={"ownerUid": owner, "label": label}, headers=world.admin_headers
    )
    return r


def _pair(client: TestClient, code: str, **body):
    return client.post("/bridge/pair", json={"code": code, "accounts": ["k@example.com"], **body})


def test_create_returns_bridge_code_and_no_secrets(client: TestClient, world: World):
    r = _create(client, world)
    assert r.status_code == 201
    body = r.json()
    assert len(body["code"]) == 8 and body["expiresAt"]
    b = body["bridge"]
    assert b["ownerUid"] == "kid" and b["ownerAlias"] == "kid" and b["paired"] is False
    assert b["label"] == "Kitchen" and b["id"].startswith("b_")
    dumped = json.dumps(body)
    assert "tokenHash" not in dumped and "fcmToken" not in dumped
    # The code pairs it.
    assert _pair(client, body["code"], simNumber=SIM, caps={"sms": True}).status_code == 200
    (listed,) = client.get("/api/family/bridges", headers=world.admin_headers).json()
    assert listed["paired"] is True and listed["simNumber"] == SIM
    assert listed["status"]["accounts"] == ["k@example.com"]
    assert "tokenHash" not in json.dumps(listed) and "fcmToken" not in json.dumps(listed)


def test_create_validates_owner(client: TestClient, world: World):
    assert _create(client, world, owner="nobody").status_code == 404
    users_store.update_user("kid", disabled=True)
    assert _create(client, world).status_code == 409
    other = families_store.create_family(name="G", created_by="root")
    users_store.create_user(uid="outsider", alias="outsider", display_name="O", family_id=other.id)
    assert _create(client, world, owner="outsider").status_code == 404
    assert client.post(
        "/api/family/bridges", json={"ownerUid": "kid", "label": ""}, headers=world.admin_headers
    ).status_code == 422
    # A member (not an admin) cannot.
    fb_auth.create_user(uid="m1", email="m1@example.com")
    users_store.create_user(uid="m1", alias="m1", display_name="M", family_id=world.family_id)
    fb_auth.set_custom_user_claims("m1", {"role": "member", "fam": world.family_id})
    r = client.get("/api/family/bridges", headers=auth_header("m1"))
    assert r.status_code in (401, 403)


def test_list_is_scoped_to_the_family(client: TestClient, world: World):
    _create(client, world)
    other = families_store.create_family(name="G", created_by="root")
    fb_auth.create_user(uid="adm2", email="adm2@example.com")
    users_store.create_user(uid="adm2", alias="adm2", display_name="a2", role="admin", family_id=other.id)
    fb_auth.set_custom_user_claims("adm2", {"role": "admin", "fam": other.id})
    assert client.get("/api/family/bridges", headers=auth_header("adm2")).json() == []


def test_new_code_only_while_unpaired(client: TestClient, world: World):
    b = _create(client, world).json()["bridge"]
    r = client.post(f"/api/family/bridges/{b['id']}/code", headers=world.admin_headers)
    assert r.status_code == 200 and len(r.json()["code"]) == 8
    assert _pair(client, r.json()["code"], simNumber=SIM).status_code == 200
    r = client.post(f"/api/family/bridges/{b['id']}/code", headers=world.admin_headers)
    assert r.status_code == 409
    assert client.post("/api/family/bridges/b_nope/code", headers=world.admin_headers).status_code == 404


def test_reassign_moves_the_number_and_rederives_both(client: TestClient, world: World, broker: FakeBrokerClient):
    users_store.create_user(uid="sis", alias="sis", display_name="Sis", family_id=world.family_id)
    make_pager_device("pgr-kid", "kid")
    make_pager_device("pgr-sis", "sis")
    bridge, _ = pair_bridge(client, world)
    assert users_store.get_user("kid").smsNumber == SIM
    before = len(broker.published)
    r = client.patch(
        f"/api/family/bridges/{bridge.id}",
        json={"ownerUid": "sis", "label": "Sis phone"},
        headers=world.admin_headers,
    )
    assert r.status_code == 200 and r.json()["ownerUid"] == "sis" and r.json()["label"] == "Sis phone"
    assert users_store.get_user("kid").smsNumber is None
    assert users_store.get_user("sis").smsNumber == SIM
    topics = {p.topic for p in broker.published[before:]}
    assert {"pager/pgr-kid/down", "pager/pgr-sis/down"} <= topics
    assert bridges_store.get(bridge.id).ownerUid == "sis"


def test_number_edit_sets_normalises_and_follows_the_sim_else_voice_rule(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world, sim=SIM)
    url = f"/api/family/bridges/{bridge.id}"
    r = client.patch(url, json={"voiceNumber": "(206) 555-0199"}, headers=world.admin_headers)
    assert r.status_code == 200 and r.json()["voiceNumber"] == VOICE and r.json()["caps"]["gvoice"] is True
    assert users_store.get_user("kid").smsNumber == SIM  # SIM wins
    r = client.patch(url, json={"simNumber": ""}, headers=world.admin_headers)
    assert r.json()["simNumber"] is None and r.json()["caps"]["sms"] is False
    assert users_store.get_user("kid").smsNumber == VOICE  # falls back to Voice
    r = client.patch(url, json={"voiceNumber": ""}, headers=world.admin_headers)
    assert users_store.get_user("kid").smsNumber is None
    assert client.patch(url, json={"simNumber": "abc"}, headers=world.admin_headers).status_code == 422


def test_number_held_by_another_member_or_bridge_is_409(client: TestClient, world: World):
    users_store.create_user(uid="sis", alias="sis", display_name="Sis", family_id=world.family_id)
    users_store.set_sms_number("sis", "+12065550777")
    bridge, _ = pair_bridge(client, world)
    url = f"/api/family/bridges/{bridge.id}"
    r = client.patch(url, json={"voiceNumber": "+12065550777"}, headers=world.admin_headers)
    assert r.status_code == 409 and "@sis" in r.json()["detail"]
    assert bridges_store.get(bridge.id).voiceNumber is None
    other, _ = pair_bridge(client, world, owner="sis", sim="+12065550888")
    # sis already holds the bridge number; owner kid wanting it => 409 too.
    r = client.patch(url, json={"voiceNumber": "+12065550888"}, headers=world.admin_headers)
    assert r.status_code == 409
    assert other.id


def test_second_bridge_for_an_owner_with_a_bridge_number_is_refused(client: TestClient, world: World):
    first, _ = pair_bridge(client, world)
    second, _ = pair_bridge(client, world, sim="+12065550888")
    assert users_store.get_user("kid").smsNumber == SIM
    assert "already uses another bridge" in bridges_store.get(second.id).status.error
    assert first.id


def test_accept_sim_after_a_sim_swap(client: TestClient, world: World):
    bridge, headers = pair_bridge(client, world)
    new_sim = "+12065550777"
    client.post("/bridge/heartbeat", json={"status": {"simNumber": new_sim}}, headers=headers)
    assert bridges_store.get(bridge.id).status.error == "SIM changed"
    r = client.post(f"/api/family/bridges/{bridge.id}/accept-sim", headers=world.admin_headers)
    assert r.status_code == 200
    body = r.json()
    assert body["simNumber"] == new_sim and body["status"]["error"] is None
    assert users_store.get_user("kid").smsNumber == new_sim
    assert users_store.get_uid_for_sms_number(SIM) is None
    # Nothing reported -> 409.
    fresh, _ = pair_bridge(client, world, owner="kid", sim=None, voice=VOICE)
    bridges_store.touch(fresh.id, {}, None)
    r = client.post(f"/api/family/bridges/{fresh.id}/accept-sim", headers=world.admin_headers)
    assert r.status_code == 409


def test_unpair_fails_pending_outbox_and_its_delivery_and_clears_the_number(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    from app.db.firestore import get_db
    from app.routing import Routing
    from app.store import allow as allow_store
    from app.store import externals as externals_store

    bridge, headers = pair_bridge(client, world)
    get_db().collection("users").document("kid").update({"policy": {"out": "people_sms", "in": "people_sms"}})
    ext = externals_store.get_or_create(world.family_id, MOM, "Mom")
    allow_store.set_edge("kid", ext.uid, message=True, locate=False)
    result = Routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="hi", origin_backend_kind="pager"
    )
    (msg,) = result.messages
    assert len(bridge_outbox.list_pending(bridge.id)) == 1

    r = client.delete(f"/api/family/bridges/{bridge.id}", headers=world.admin_headers)
    assert r.status_code == 200
    body = r.json()
    assert body["paired"] is False and body["status"]["unpaired"] is True
    assert "tokenHash" not in json.dumps(body)
    assert bridge_outbox.list_pending(bridge.id) == []
    item = bridge_outbox.get(bridge.id, f"ob_{msg.id}_sms")
    assert item.state == "failed" and item.reason == "unpaired"
    d = messages_store.get_message(msg.id).deliveries["sms"]
    assert d.state == "failed" and d.error == "unpaired"
    assert users_store.get_user("kid").smsNumber is None
    assert users_store.get_user_by_alias(ext.alias) is not None  # contact kept
    # The token no longer works, and a re-pair works with a new code.
    assert client.post("/bridge/heartbeat", json={"status": {}}, headers=headers).status_code == 401
    code = client.post(f"/api/family/bridges/{bridge.id}/code", headers=world.admin_headers).json()["code"]
    assert _pair(client, code, simNumber=SIM, caps={"sms": True}).status_code == 200
    assert users_store.get_user("kid").smsNumber == SIM
    # Idempotent on an already-unpaired bridge.
    assert client.delete(f"/api/family/bridges/{bridge.id}", headers=world.admin_headers).status_code == 200


def test_other_familys_bridge_is_404_everywhere(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world)
    other = families_store.create_family(name="G", created_by="root")
    fb_auth.create_user(uid="adm2", email="adm2@example.com")
    users_store.create_user(uid="adm2", alias="adm2", display_name="a2", role="admin", family_id=other.id)
    fb_auth.set_custom_user_claims("adm2", {"role": "admin", "fam": other.id})
    h = auth_header("adm2")
    base = f"/api/family/bridges/{bridge.id}"
    assert client.patch(base, json={"label": "x"}, headers=h).status_code == 404
    assert client.delete(base, headers=h).status_code == 404
    assert client.post(f"{base}/code", headers=h).status_code == 404
    assert client.post(f"{base}/accept-sim", headers=h).status_code == 404
    assert client.post(f"{base}/inspect", json={"link": "https://chat.google.com/x"}, headers=h).status_code == 404
    assert get_db() is not None

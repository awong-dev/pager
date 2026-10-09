"""Bridge outbox, the `bridge` backend kind and the `sms` backend's bridge
transport -- docs/BRIDGE_PHONE_DESIGN.md decisions 4, 7 (backend row), 11, O1/O2."""

from __future__ import annotations

import logging
from datetime import UTC, datetime, timedelta

from fastapi.testclient import TestClient

from app import jobs
from app.db.firestore import get_db
from app.routing import Routing
from app.store import backends as backends_store
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import externals as externals_store
from app.store import messages as messages_store
from app.tasks import InlineTaskQueue
from tests.bridge_world import (  # noqa: F401
    MOM,
    SIM,
    VOICE,
    RecordingFCM,
    World,
    broker,
    client,
    fcm,
    pair_bridge,
    world,
)
from tests.fake_transport import FakeBrokerClient


def _contact(world: World, phone: str = MOM, name: str = "Mom"):
    from app.store import allow as allow_store

    get_db().collection("users").document("kid").update(
        {"policy": {"out": "people_sms", "in": "people_sms"}}
    )
    ext = externals_store.get_or_create(world.family_id, phone, name)
    allow_store.set_edge("kid", ext.uid, message=True, locate=False)
    return ext


def _send(broker: FakeBrokerClient, alias: str, body: str = "hi", wire: str | None = None):
    result = Routing(broker).send(
        sender_uid="kid",
        recipient_alias=alias,
        kind="text",
        body=body,
        origin_backend_kind="pager",
        wire_id=wire,
    )
    assert result.rejected == [], result.rejected
    return result.messages[0]


def _delivery(msg_id: str):
    (d,) = messages_store.get_message(msg_id).deliveries.values()
    return d


# ---------------------------------------------------------------------------
# store
# ---------------------------------------------------------------------------


def test_enqueue_is_idempotent_on_the_deterministic_id(client: TestClient, world: World, fcm: RecordingFCM):
    bridge, _ = pair_bridge(client, world)
    bridges_store.touch(bridge.id, {}, "fcm-tok")
    bridge = bridges_store.get(bridge.id)
    a = bridge_outbox.enqueue_send(bridge, "m_1", "sms", source="sms", to={"phone": MOM}, text="x")
    b = bridge_outbox.enqueue_send(bridge, "m_1", "sms", source="sms", to={"phone": MOM}, text="x")
    assert a.id == b.id == "ob_m_1_sms" and a.state == b.state == "pending"
    assert len(bridge_outbox.list_pending(bridge.id)) == 1
    # FCM data only for the new row.
    assert fcm.calls == [(["fcm-tok"], {"kind": "outbox", "bridgeId": bridge.id})]


def test_hint_and_inspect_ids_and_no_fcm_without_token(client: TestClient, world: World, fcm: RecordingFCM):
    bridge, _ = pair_bridge(client, world)
    h1 = bridge_outbox.enqueue_hint(bridge, source="sms", phone=MOM, text="t", wire_id="br_b_e1")
    h2 = bridge_outbox.enqueue_hint(bridge, source="sms", phone=MOM, text="t", wire_id="br_b_e1")
    assert h1.id == h2.id == "ob_h_br_b_e1" and h1.msgId is None
    i = bridge_outbox.enqueue_inspect(bridge, "https://chat.google.com/x")
    assert i.id.startswith("ob_") and i.kind == "inspect" and i.to == {"link": "https://chat.google.com/x"}
    assert fcm.calls == []


def test_ack_transitions_once_and_unknown_is_none(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world)
    item = bridge_outbox.enqueue_send(bridge, "m_1", "sms", source="sms", to={}, text="x")
    got, moved = bridge_outbox.ack(bridge.id, item.id, "sent", None, 1)
    assert moved and got.state == "sent" and got.tier == 1 and got.ackedAt is not None
    got2, moved2 = bridge_outbox.ack(bridge.id, item.id, "failed", "late", 1)
    assert not moved2 and got2.state == "sent"
    assert bridge_outbox.ack(bridge.id, "ob_nope", "sent", None, 1) is None


def test_list_pending_oldest_first_capped(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world)
    for i in range(25):
        bridge_outbox.enqueue_send(bridge, f"m_{i:02d}", "sms", source="sms", to={}, text=str(i))
    items = bridge_outbox.list_pending(bridge.id)
    assert len(items) == 20
    assert [i.text for i in items] == [str(n) for n in range(20)]


# ---------------------------------------------------------------------------
# the `sms` backend's bridge transport
# ---------------------------------------------------------------------------


def test_bridge_transport_sends_the_raw_body_and_stays_queued(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch, caplog
):
    caplog.set_level(logging.INFO, logger="relay.backends.sms")
    bridge, _ = pair_bridge(client, world)
    ext = _contact(world)
    msg = _send(broker, ext.alias, "on my way")
    d = _delivery(msg.id)
    assert d.state == "queued" and d.externalId == f"ob_{msg.id}_sms"
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.text == "on my way"  # raw body: no wrapper, no disclosure
    assert item.source == "sms" and item.to == {"phone": MOM}
    assert item.msgId == msg.id and item.bid == "sms"
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("sms out"))
    assert line == f"sms out to=...0111 from=...0100 sid=ob_{msg.id}_sms status=queued code=bridge"


def test_redeliver_twice_leaves_one_outbox_row(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch
):
    bridge, _ = pair_bridge(client, world)
    ext = _contact(world)
    msg = _send(broker, ext.alias)
    routing = Routing(broker)
    assert routing.redeliver(msg, "sms") and routing.redeliver(msg, "sms")
    assert len(bridge_outbox.list_pending(bridge.id)) == 1
    assert _delivery(msg.id).state == "queued"


def test_ack_sent_marks_delivery_sent_and_reack_reapplies(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch
):
    bridge, headers = pair_bridge(client, world)
    ext = _contact(world)
    msg = _send(broker, ext.alias)
    ob = f"ob_{msg.id}_sms"
    r = client.post(f"/bridge/outbox/{ob}/ack", json={"state": "sent", "tier": 1}, headers=headers)
    assert r.status_code == 204
    assert _delivery(msg.id).state == "sent"
    # Simulate a lost delivery write, then the phone's retry.
    get_db().collection("messages").document(msg.id).update({"deliveries.sms.state": "queued"})
    r = client.post(f"/bridge/outbox/{ob}/ack", json={"state": "sent", "tier": 1}, headers=headers)
    assert r.status_code == 204 and _delivery(msg.id).state == "sent"
    assert bridges_store.get(bridge.id).status.tier2Count == 0


def test_ack_failed_sets_error_and_tier2_counts_once(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch
):
    bridge, headers = pair_bridge(client, world)
    ext = _contact(world)
    msg = _send(broker, ext.alias)
    ob = f"ob_{msg.id}_sms"
    body = {"state": "failed", "reason": "sms_1", "tier": 2}
    assert client.post(f"/bridge/outbox/{ob}/ack", json=body, headers=headers).status_code == 204
    assert client.post(f"/bridge/outbox/{ob}/ack", json=body, headers=headers).status_code == 204
    d = _delivery(msg.id)
    assert d.state == "failed" and d.error == "sms_1"
    assert bridges_store.get(bridge.id).status.tier2Count == 1
    assert client.post("/bridge/outbox/ob_nope/ack", json=body, headers=headers).status_code == 404


def test_outbox_from_another_bridge_is_invisible(client: TestClient, world: World):
    bridge, _headers = pair_bridge(client, world)
    other, other_headers = pair_bridge(client, world, sim="+12065550888")
    bridge_outbox.enqueue_send(bridge, "m_1", "sms", source="sms", to={}, text="x")
    assert client.get("/bridge/outbox", headers=other_headers).json() == {"items": []}
    r = client.post(
        "/bridge/outbox/ob_m_1_sms/ack", json={"state": "sent", "tier": 1}, headers=other_headers
    )
    assert r.status_code == 404
    assert other.id != bridge.id


def test_a_number_that_is_not_a_bridge_number_fails_no_bridge(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    from app.store import users as users_store

    users_store.set_sms_number("kid", "+12065550444")
    ext = _contact(world)
    msg = _send(broker, ext.alias, "hello")
    d = _delivery(msg.id)
    assert d.state == "failed" and d.error == "no_bridge"
    assert bridge_outbox.list_pending("b_none") == []


# ---------------------------------------------------------------------------
# Voice-only (O1 revised)
# ---------------------------------------------------------------------------


def test_voice_only_bridge_send_lands_as_gvoice_with_the_link(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch
):
    bridge, _ = pair_bridge(client, world, sim=None, voice=VOICE)
    ext = _contact(world)
    msg = _send(broker, ext.alias, "hi grandma")
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.source == "gvoice"
    assert item.to["phone"] == MOM
    assert item.to["link"] == f"https://voice.google.com/u/0/messages?itemId=t.{MOM}"
    assert item.text == "hi grandma" and _delivery(msg.id).state == "queued"


def test_via_entry_picks_the_channel_and_voice_conversation(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch
):
    bridge, _ = pair_bridge(client, world, sim=SIM, voice=VOICE)
    ext = _contact(world)
    backends_store.update_backend(
        ext.uid,
        "sms",
        config={"phone": MOM, "via": {"kid": "gvoice"}, "voiceConv": {"kid": "conv-9"}},
    )
    _send(broker, ext.alias)
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.source == "gvoice" and item.to["conversationId"] == "conv-9"
    # Another member's entry does not apply: kid's own default is the SIM.
    backends_store.update_backend(ext.uid, "sms", config={"phone": MOM, "via": {"sis": "gvoice"}})
    msg2 = _send(broker, ext.alias, "second")
    ob2 = bridge_outbox.get(bridge.id, f"ob_{msg2.id}_sms")
    assert ob2.source == "sms"


# ---------------------------------------------------------------------------
# bridge backend kind
# ---------------------------------------------------------------------------


def test_bridge_backend_kind_is_external_only_and_has_kind():
    from app.store import users as users_store

    users_store.create_user(uid="p1", alias="p1", display_name="P")
    get_db().collection("users").document("p1").collection("backends").document("bridge").set(
        {"kind": "bridge", "config": {"bridgeId": "b"}, "enabled": True}
    )
    assert backends_store.get_backend("p1", "bridge") is None
    assert not backends_store.has_kind("p1", "bridge")
    users_store.create_user(
        uid="x_1", alias="x1", display_name="X", family_id=None, kind="external", owner_family_id="f"
    )
    ext = users_store.get_user("x_1")
    externals_store.ensure_bridge_backend(ext, {"bridgeId": "b", "source": "gchat"})
    externals_store.ensure_bridge_backend(ext, {"bridgeId": "b", "source": "gchat", "link": "l"})
    assert backends_store.has_kind("x_1", "bridge")
    (row,) = backends_store.list_backends("x_1")
    assert row.id == "bridge" and row.config["link"] == "l"


def test_bridge_backend_deliver_enqueues_and_unpaired_fails(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    from app.backends.bridge import BridgeBackend
    from app.store import users as users_store

    bridge, _ = pair_bridge(client, world)
    users_store.create_user(
        uid="x_c1", alias="xc1", display_name="Soccer", family_id=None, kind="external",
        owner_family_id=world.family_id,
    )
    ext = users_store.get_user("x_c1")
    cfg = {"bridgeId": bridge.id, "source": "gchat", "conversationId": "spaces/AAA", "link": "https://chat.google.com/room/AAA"}
    externals_store.ensure_bridge_backend(ext, cfg)
    m = messages_store.create_message(
        sender_uid="kid", recipient_uid="x_c1", kind="text", ts=1, body="go team",
        deliveries={"bridge": {"kind": "bridge", "state": "queued", "attempts": 0}},
    )
    row = backends_store.get_backend("x_c1", "bridge")
    out = BridgeBackend().deliver(m, messages_store.get_message(m.id).deliveries["bridge"], row)
    assert out.ok and out.state == "queued"
    items = bridge_outbox.list_pending(bridge.id)
    assert items[0].to == {"conversationId": "spaces/AAA", "link": cfg["link"]}
    assert BridgeBackend().render_state(messages_store.get_message(m.id).deliveries["bridge"]) == "waiting for the phone"
    bridges_store.unpair(bridge.id)
    m2 = messages_store.create_message(
        sender_uid="kid", recipient_uid="x_c1", kind="text", ts=2, body="again",
        deliveries={"bridge": {"kind": "bridge", "state": "queued", "attempts": 0}},
    )
    out2 = BridgeBackend().deliver(m2, messages_store.get_message(m2.id).deliveries["bridge"], row)
    assert not out2.ok and out2.error == "no_bridge"
    assert messages_store.get_message(m2.id).deliveries["bridge"].state == "failed"


# ---------------------------------------------------------------------------
# long poll, heartbeat pending, tick, sweep
# ---------------------------------------------------------------------------


def test_outbox_long_poll_returns_at_once_when_an_item_exists_and_waits_otherwise(
    client: TestClient, world: World
):
    import time

    bridge, headers = pair_bridge(client, world)
    start = time.monotonic()
    assert client.get("/bridge/outbox?wait=1", headers=headers).json() == {"items": []}
    assert 0.9 <= time.monotonic() - start < 3
    bridge_outbox.enqueue_send(bridge, "m_1", "sms", source="sms", to={"phone": MOM}, text="x")
    start = time.monotonic()
    r = client.get("/bridge/outbox?wait=25", headers=headers)
    assert time.monotonic() - start < 3
    (item,) = r.json()["items"]
    assert item["id"] == "ob_m_1_sms" and item["text"] == "x" and item["kind"] == "send"
    hb = client.post("/bridge/heartbeat", json={"status": {}}, headers=headers)
    assert hb.json() == {"pending": 1}
    assert client.get("/bridge/outbox", headers={}).status_code == 401


def test_tick_fails_a_stale_row_and_its_delivery(
    client: TestClient, world: World, broker: FakeBrokerClient, monkeypatch
):
    bridge, _ = pair_bridge(client, world)
    ext = _contact(world)
    msg = _send(broker, ext.alias)
    old = datetime.now(UTC) - timedelta(hours=25)
    get_db().collection("bridges").document(bridge.id).collection("outbox").document(
        f"ob_{msg.id}_sms"
    ).update({"createdAt": old})
    # An unpaired bridge's stale row fails too.
    gone = bridges_store.create("kid", world.family_id, "gone", "adm")
    bridge_outbox.enqueue_inspect(gone, "https://chat.google.com/x")
    get_db().collection("bridges").document(gone.id).collection("outbox").document(
        bridge_outbox.list_pending(gone.id)[0].id
    ).update({"createdAt": old})

    result = jobs.tick(Routing(broker), task_queue=InlineTaskQueue())
    assert result.bridgeOutboxFailed == 2
    d = _delivery(msg.id)
    assert d.state == "failed" and d.error == "bridge_offline"
    assert bridge_outbox.list_pending(bridge.id) == []


def test_tick_does_not_touch_bridge_queued_sms(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    bridge, _ = pair_bridge(client, world)
    ext = _contact(world)
    for i in range(11):
        _send(broker, ext.alias, f"b{i}")
    jobs.tick(Routing(broker), task_queue=InlineTaskQueue())
    assert len(bridge_outbox.list_pending(bridge.id)) == 11


def test_sweep_deletes_old_acked_rows_only(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world)
    a = bridge_outbox.enqueue_send(bridge, "m_a", "sms", source="sms", to={}, text="a")
    b = bridge_outbox.enqueue_send(bridge, "m_b", "sms", source="sms", to={}, text="b")
    bridge_outbox.enqueue_send(bridge, "m_c", "sms", source="sms", to={}, text="c")
    for it in (a, b):
        bridge_outbox.ack(bridge.id, it.id, "sent", None, 1)
    get_db().collection("bridges").document(bridge.id).collection("outbox").document(a.id).update(
        {"ackedAt": datetime.now(UTC) - timedelta(days=8)}
    )
    result = jobs.sweep()
    assert result.bridgeOutboxDeleted == 1
    assert bridge_outbox.get(bridge.id, a.id) is None
    assert bridge_outbox.get(bridge.id, b.id) is not None
    assert len(bridge_outbox.list_pending(bridge.id)) == 1


def test_self_service_cannot_create_a_bridge_backend(client: TestClient, world: World):
    from tests.firebase_test_utils import auth_header

    r = client.post(
        "/api/me/backends",
        json={"kind": "bridge", "config": {}},
        headers=auth_header("adm"),
    )
    assert r.status_code == 422

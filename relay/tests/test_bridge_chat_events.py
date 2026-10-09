"""Google Chat events: seen rows, held chat, the `chat_unknown` alert --
docs/BRIDGE_PHONE_DESIGN.md decisions 5 (gchat), 6, 8."""

from __future__ import annotations

from fastapi.testclient import TestClient

from app.db.firestore import get_db
from app.store import alerts as alerts_store
from app.store import bridge_conversations as conv_store
from app.store import held_chat as held_chat_store
from tests.bridge_world import (  # noqa: F401
    RecordingFCM,
    World,
    broker,
    client,
    fcm,
    pair_bridge,
    world,
)

_n = 0
GROUP = "spaces/AAA|soccer"  # a `/` and `|` on purpose: ids are hashed into refs


def _ev(sender: str = "Dana P", text: str = "who is driving?", conv: str = GROUP, **extra):
    global _n
    _n += 1
    return {
        "id": f"g{_n}",
        "source": "gchat",
        "conversation": {"id": conv, "title": "Soccer carpool", "isGroup": True},
        "sender": {"name": sender},
        "text": text,
        "ts": 1,
        **extra,
    }


def _post(client: TestClient, headers, *events):
    r = client.post("/bridge/events", json={"events": list(events)}, headers=headers)
    assert r.status_code == 200, r.text
    return [x["outcome"] for x in r.json()["results"]]


def _alerts(world: World):
    return alerts_store.list_alerts(world.family_id, "open")


def test_first_message_from_an_unknown_group_raises_one_alert(
    client: TestClient, world: World, fcm: RecordingFCM
):
    bridge, headers = pair_bridge(client, world)
    assert _post(client, headers, _ev()) == ["held"]
    (alert,) = _alerts(world)
    assert alert.kind == "chat_unknown" and alert.people == ["Dana P"] and alert.heldCount == 1
    assert alert.bridgeId == bridge.id and alert.conversationId == GROUP
    assert alert.convRef == conv_store.conv_ref(GROUP) and alert.convTitle == "Soccer carpool"
    assert alert.isGroup is True and alert.source == "gchat" and alert.subjectAlias == "kid"
    (call,) = fcm.calls
    assert call[1]["title"] == "Google Chat for @kid: Soccer carpool"
    assert call[1]["body"] == "Soccer carpool (Dana P) → @kid: who is driving?"
    row = conv_store.get(bridge.id, GROUP)
    assert row.status == "seen" and row.heldCount == 1 and row.alertId == alert.id
    assert row.id == f"{bridge.id}_{conv_store.conv_ref(GROUP)}"


def test_second_message_updates_the_same_alert(client: TestClient, world: World, fcm: RecordingFCM):
    _, headers = pair_bridge(client, world)
    _post(client, headers, _ev("Dana P", "first"))
    assert _post(client, headers, _ev("Lee", "second")) == ["held"]
    (alert,) = _alerts(world)
    assert alert.heldCount == 2 and alert.people == ["Dana P", "Lee"] and alert.preview == "second"
    assert len(list(get_db().collection("families").document(world.family_id).collection("alerts").stream())) == 1
    assert len(fcm.calls) == 2
    # Same sender again does not duplicate the name.
    _post(client, headers, _ev("Lee", "third"))
    assert _alerts(world)[0].people == ["Dana P", "Lee"]


def test_inspect_event_creates_a_seen_row_and_no_alert(client: TestClient, world: World, fcm: RecordingFCM):
    bridge, headers = pair_bridge(client, world)
    ev = _ev(text="")
    ev["kind"] = "inspect"
    ev["conversation"]["link"] = "https://chat.google.com/room/AAA"
    ev["people"] = ["Dana P", "Lee", "Sam"]
    assert _post(client, headers, ev) == ["inspected"]
    row = conv_store.get(bridge.id, GROUP)
    assert row.status == "seen" and row.inspectedAt is not None and row.link.endswith("AAA")
    assert row.people == ["Dana P", "Lee", "Sam"] and row.heldCount == 0
    assert _alerts(world) == [] and fcm.calls == []
    assert list(get_db().collection("heldChat").stream()) == []


def test_ignored_and_paused_rows_drop_and_store_nothing(client: TestClient, world: World):
    bridge, headers = pair_bridge(client, world)
    _post(client, headers, _ev())
    rid = conv_store.row_id(bridge.id, GROUP)
    conv_store.set_status(rid, "ignored")
    before = len(list(get_db().collection("heldChat").stream()))
    assert _post(client, headers, _ev(text="again")) == ["dropped_ignored"]
    assert len(list(get_db().collection("heldChat").stream())) == before
    assert conv_store.get(bridge.id, GROUP).lastPreview == "who is driving?"
    conv_store.set_status(rid, "paused")
    assert _post(client, headers, _ev(text="again")) == ["dropped_paused"]


def test_held_cap_is_25_and_duplicates_are_detected(client: TestClient, world: World):
    bridge, headers = pair_bridge(client, world)
    first = _ev()
    assert _post(client, headers, first) == ["held"]
    assert _post(client, headers, first) == ["duplicate"]
    for _ in range(24):
        assert _post(client, headers, _ev()) == ["held"]
    assert _post(client, headers, _ev()) == ["held_cap"]
    rid = conv_store.row_id(bridge.id, GROUP)
    assert held_chat_store.count_held(rid) == 25
    assert held_chat_store.exists(f"br_{bridge.id}_{first['id']}")


def test_new_conversation_cap_is_50_per_day(client: TestClient, world: World):
    _, headers = pair_bridge(client, world)
    for i in range(50):
        assert _post(client, headers, _ev(conv=f"conv-{i}")) == ["held"]
    assert _post(client, headers, _ev(conv="conv-50")) == ["dropped_conv_cap"]
    # A known conversation still works.
    assert _post(client, headers, _ev(conv="conv-0")) == ["held"]


def test_caps_and_empty_and_attachment_placeholder(client: TestClient, world: World):
    _, headers = pair_bridge(client, world, caps={"sms": True, "gchat": False, "gvoice": False})
    assert _post(client, headers, _ev()) == ["dropped_cap"]
    _, headers2 = pair_bridge(client, world, sim="+12065550888")
    assert _post(client, headers2, _ev(text="")) == ["dropped_empty"]
    assert _post(client, headers2, _ev(text="", attachments=[{"kind": "image"}])) == ["held"]
    (held,) = list(get_db().collection("heldChat").stream())
    assert held.to_dict()["body"] == "[photo]"


def test_dismiss_marks_held_rows_and_a_later_text_raises_a_fresh_alert(
    client: TestClient, world: World
):
    bridge, headers = pair_bridge(client, world)
    _post(client, headers, _ev("Dana P", "one"))
    _post(client, headers, _ev("Dana P", "two"))
    (alert,) = _alerts(world)
    r = client.post(f"/api/family/alerts/{alert.id}/dismiss", headers=world.admin_headers)
    assert r.status_code == 200 and r.json()["status"] == "dismissed"
    rows = held_chat_store.list_for_conversation(conv_store.row_id(bridge.id, GROUP), status=None)
    assert [x.status for x in rows] == ["dismissed", "dismissed"]
    assert conv_store.get(bridge.id, GROUP).status == "seen"
    _post(client, headers, _ev("Dana P", "three"))
    (fresh,) = _alerts(world)
    assert fresh.id != alert.id and fresh.heldCount == 1


def test_block_and_approve_are_400_for_chat_unknown(client: TestClient, world: World):
    _, headers = pair_bridge(client, world)
    _post(client, headers, _ev())
    (alert,) = _alerts(world)
    r = client.post(f"/api/family/alerts/{alert.id}/block", headers=world.admin_headers)
    assert r.status_code == 400 and "Ignore" in r.json()["detail"]
    r = client.post(f"/api/family/alerts/{alert.id}/approve", json={}, headers=world.admin_headers)
    assert r.status_code == 400
    assert alerts_store.get(world.family_id, alert.id).status == "open"


def test_sweep_deletes_old_held_chat(client: TestClient, world: World):
    from datetime import UTC, datetime, timedelta

    from app import jobs

    bridge, headers = pair_bridge(client, world)
    _post(client, headers, _ev())
    (snap,) = list(get_db().collection("heldChat").stream())
    snap.reference.update({"receivedAt": datetime.now(UTC) - timedelta(days=400)})
    assert jobs.sweep().heldChatDeleted == 1
    assert bridge.id

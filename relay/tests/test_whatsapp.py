"""WhatsApp as a bridge source (WA1-WA5, 9 Oct 2026): caps, DM text path,
group conversations, outbox shapes, channel choice and the inspect refusal."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.db.firestore import get_db
from app.routing import Routing
from app.store import alerts as alerts_store
from app.store import backends as backends_store
from app.store import bridge_conversations as conv_store
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import externals as externals_store
from app.store import held_chat as held_chat_store
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
from tests.test_bridge_events import _kid_setup, _mom, _post, _sms_config
from tests.test_chat_subscribe import Env

WA_CAPS = {"sms": True, "gchat": True, "gvoice": True, "whatsapp": True}
JID = f"{MOM.lstrip('+')}@s.whatsapp.net"
GJID = "120363025@g.us"
_n = 0


def _dm(*, phone: str | None = MOM, text: str = "hi", conv: str = JID, **extra):
    global _n
    _n += 1
    sender = {"name": "Mom"}
    if phone is not None:
        sender["phone"] = phone
    return {
        "id": f"w{_n}",
        "source": "whatsapp",
        "conversation": {"id": conv, "isGroup": False},
        "sender": sender,
        "text": text,
        "ts": 1,
        **extra,
    }


def _grp(sender: str = "~ Dana P", text: str = "who drives?", **extra):
    global _n
    _n += 1
    return {
        "id": f"wg{_n}",
        "source": "whatsapp",
        "conversation": {"id": GJID, "title": "Soccer", "isGroup": True},
        "sender": {"name": sender},
        "text": text,
        "ts": 1,
        **extra,
    }


# --- WA1: caps -------------------------------------------------------------


def test_pair_stores_whatsapp_cap_and_status(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world, caps=WA_CAPS)
    assert bridge.caps.whatsapp and bridge.status.whatsapp
    plain, _ = pair_bridge(client, world, sim=None, voice=None, caps={"gchat": True})
    assert not plain.caps.whatsapp and not plain.status.whatsapp


def test_heartbeat_status_whatsapp_updates_caps_and_absence_keeps_them(
    client: TestClient, world: World
):
    bridge, headers = pair_bridge(client, world, caps={"sms": True, "gchat": True})
    assert not bridge.caps.whatsapp
    hb = lambda status: client.post("/bridge/heartbeat", json={"status": status}, headers=headers)
    assert hb({"whatsapp": True}).status_code == 200
    got = bridges_store.get(bridge.id)
    assert got.caps.whatsapp and got.status.whatsapp and got.caps.sms and got.caps.gchat
    hb({"battery": 50})
    assert bridges_store.get(bridge.id).caps.whatsapp
    hb({"smsDefault": True})  # an sms-only update must not drop whatsapp
    assert bridges_store.get(bridge.id).caps.whatsapp
    hb({"whatsapp": False})
    got = bridges_store.get(bridge.id)
    assert not got.caps.whatsapp and not got.status.whatsapp


def test_number_patch_and_accept_sim_keep_whatsapp(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world, caps=WA_CAPS)
    r = client.patch(
        f"/api/family/bridges/{bridge.id}", json={"voiceNumber": VOICE}, headers=world.admin_headers
    )
    assert r.status_code == 200 and r.json()["caps"]["whatsapp"] is True
    assert bridges_store.get(bridge.id).caps.whatsapp
    bridges_store.touch(bridge.id, {"simNumber": "+12065550777"}, None)
    r = client.post(f"/api/family/bridges/{bridge.id}/accept-sim", headers=world.admin_headers)
    assert r.status_code == 200 and r.json()["caps"]["whatsapp"] is True


# --- B13: DMs keyed by conversation id (BRIDGE_WHATSAPP_LID_DESIGN) -------------

LID = "9876543210@lid"


def _send(broker: FakeBrokerClient, ext, wire: str):
    return Routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="yo",
        origin_backend_kind="pager", wire_id=wire,
    )


def _lid(text: str = "hi", **extra):
    return _dm(phone=None, text=text, conv=LID, **{"sender": {"name": "Albert"}, **extra})


@pytest.fixture
def env(client: TestClient, world: World, broker: FakeBrokerClient) -> Env:
    e = Env(client, world, broker)
    bridges_store.set_caps(e.bridge.id, bridges_store.BridgeCaps(**WA_CAPS))
    return e


def test_lid_dm_without_phone_or_title_is_held_with_a_chat_unknown_alert(env: Env, fcm: RecordingFCM):
    assert env.events(_lid("hello")) == ["held"]
    row = conv_store.get(env.bridge.id, LID)
    assert row.source == "whatsapp" and row.title == "Albert" and row.isGroup is False
    (alert,) = alerts_store.list_alerts(env.world.family_id, "open")
    assert alert.kind == "chat_unknown" and alert.source == "whatsapp"
    assert [a for a in alerts_store.list_alerts(env.world.family_id, "open") if a.kind == "sms_unknown"] == []
    bodies = [str(p) for p in fcm.sent] if hasattr(fcm, "sent") else []
    assert not bodies or any("WhatsApp: Albert \u2192" in b for b in bodies)


def test_dm_title_prefers_conversation_title_and_empty_sender_is_someone(env: Env):
    ev = _lid("x")
    ev["conversation"]["title"] = "Albert W"
    ev["sender"] = {"name": ""}
    assert env.events(ev) == ["held"]
    assert conv_store.get(env.bridge.id, LID).title == "Albert W"


def test_same_event_twice_is_duplicate_and_held_once(env: Env):
    ev = _lid("once")
    assert env.events(ev, ev) == ["held", "duplicate"]
    assert conv_store.get(env.bridge.id, LID).heldCount == 1


def test_lid_subscribe_delivers_backlog_book_entry_is_chat_and_next_is_delivered(env: Env):
    env.events(_lid("one"), _lid("two"))
    out = env.sub(LID, {"pagerName": "Albert WA"})
    assert out["delivered"] == 2 and out["conversation"]["source"] == "whatsapp"
    pages = [d for d in env.downs() if d.get("kind") != "book" and "body" in d]
    assert [p["body"] for p in pages] == ["one", "two"]
    from app import devcfg

    assert {"a": out["alias"], "n": "Albert WA", "t": "chat"} in devcfg._approved_contacts("kid")
    assert env.events(_lid("three")) == ["delivered"]


def test_lid_pager_reply_outbox_has_conversation_id_and_title_but_no_phone(env: Env):
    env.events(_lid("hello"))
    out = env.sub(LID, {"pagerName": "Albert WA"})
    env.pager_up(out["alias"], "hi back", "u_lid1")
    (item,) = bridge_outbox.list_pending(env.bridge.id)
    assert item.source == "whatsapp" and item.text == "hi back"
    assert item.to["conversationId"] == LID and item.to["title"] == "Albert"
    assert "phone" not in item.to and item.to.get("link") is None


def test_phone_jid_dm_takes_the_chat_path_and_the_reply_carries_the_link(env: Env):
    link = f"https://wa.me/{MOM.lstrip('+')}"
    ev = _dm(phone=MOM, conv=JID)
    ev["conversation"]["link"] = link
    assert env.events(ev) == ["held"]
    alerts = alerts_store.list_alerts(env.world.family_id, "open")
    assert [a.kind for a in alerts] == ["chat_unknown"]
    out = env.sub(JID, {"pagerName": "Mom WA"})
    env.pager_up(out["alias"], "yo", "u_jid1")
    (item,) = bridge_outbox.list_pending(env.bridge.id)
    assert item.to["link"] == link and item.to["conversationId"] == JID and "phone" not in item.to


def test_dm_needs_the_whatsapp_cap(client: TestClient, world: World, broker: FakeBrokerClient):
    _, no_wa = pair_bridge(client, world, caps={"sms": True})
    assert _post(client, no_wa, _lid()) == ["dropped_cap"]


@pytest.mark.parametrize("conv", ["com.whatsapp|42", "123@c.us", "abc@lid", "@lid", "123@g.us.x"])
def test_bad_conversation_ids_are_dropped_bad_conv(env: Env, conv: str):
    assert env.events(_dm(conv=conv)) == ["dropped_bad_conv"]
    assert conv_store.get(env.bridge.id, conv) is None


def test_sms_and_voice_without_a_number_are_still_dropped_bad_from(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world, voice=VOICE, caps=WA_CAPS)
    for src in ("sms", "gvoice"):
        ev = _dm(phone=None, conv="thread-1")
        ev["source"] = src
        assert _post(client, headers, ev) == ["dropped_bad_from"]


def test_whatsapp_is_not_an_sms_channel_for_a_contact_with_a_stale_via(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    bridge, _ = pair_bridge(client, world, caps=WA_CAPS)
    ext = _mom(world)
    bid = externals_store.ensure_sms_backend(ext)
    row = backends_store.get_backend(ext.uid, bid)
    backends_store.update_backend(ext.uid, bid, config={**row.config, "via": {"kid": "whatsapp"}})
    _send(broker, ext, "u_stale")
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.source == "sms" and item.to == {"phone": MOM}


# --- WA3: groups ---------------------------------------------------------------


def test_group_unknown_alert_strips_the_tilde_and_needs_the_cap(
    client: TestClient, world: World, broker: FakeBrokerClient, fcm: RecordingFCM
):
    bridge, headers = pair_bridge(client, world, caps={"sms": True})
    assert _post(client, headers, _grp()) == ["dropped_cap"]
    bridges_store.set_caps(bridge.id, bridges_store.BridgeCaps(**WA_CAPS))
    assert _post(client, headers, _grp("~ Dana P"), _grp("~Lee"), _grp("Sam")) == ["held"] * 3
    (alert,) = alerts_store.list_alerts(world.family_id, "open")
    assert alert.kind == "chat_unknown" and alert.source == "whatsapp" and alert.isGroup is True
    assert alert.people == ["Dana P", "Lee", "Sam"]
    row = conv_store.get(bridge.id, GJID)
    assert row.source == "whatsapp" and row.title == "Soccer" and row.heldCount == 3
    assert {h.senderName for h in held_chat_store.list_for_conversation(row.id)} == {"Dana P", "Lee", "Sam"}


def test_group_subscribe_ignore_and_reply_outbox_shape(env: Env):
    env.events(_grp("~ Dana P", "who drives?"), _grp("~ Lee", "me"))
    out = env.sub(
        GJID,
        {"pagerName": "Soccer", "canReply": True,
         "roster": [{"name": "Dana P", "nick": "dana"}, {"name": "Lee", "nick": "lee"}]},
    )
    assert out["delivered"] == 2 and out["conversation"]["source"] == "whatsapp"
    pages = [d for d in env.downs() if d.get("kind") != "book" and "body" in d]
    assert [(p["sndr"], p["body"]) for p in pages] == [("dana", "who drives?"), ("lee", "me")]
    assert env.events(_grp("~ Dana P", "live")) == ["delivered"]
    env.pager_up(out["alias"], "I can", "u_wg1")
    items = bridge_outbox.list_pending(env.bridge.id)
    (item,) = items
    assert item.source == "whatsapp" and item.replyHint == GJID
    assert item.to["conversationId"] == GJID and item.to["title"] == "Soccer"
    assert item.text == "I can"


def test_group_ignore_drops_later_messages(env: Env):
    env.events(_grp())
    r = env.client.post(
        f"/api/family/bridges/{env.bridge.id}/conversations/{conv_store.conv_ref(GJID)}/ignore",
        headers=env.world.admin_headers,
    )
    assert r.status_code == 200, r.text
    assert env.events(_grp()) == ["dropped_ignored"]


def test_whatsapp_group_event_does_not_take_the_dm_text_path(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world, caps=WA_CAPS)
    ev = _grp()
    ev["sender"]["phone"] = MOM
    assert _post(client, headers, ev) == ["held"]  # held chat, not dropped_group


# --- WA3: inspect --------------------------------------------------------------


@pytest.mark.parametrize(
    "link",
    [
        "https://wa.me/12065550111",
        "https://chat.whatsapp.com/AbCdEf",
        "https://api.whatsapp.com/send?phone=1",
        "https://www.whatsapp.com/x",
        "whatsapp://send?phone=1",
        "HTTPS://WA.ME/1",
    ],
)
def test_inspect_by_whatsapp_link_is_400(client: TestClient, world: World, link: str):
    bridge, _ = pair_bridge(client, world, caps=WA_CAPS)
    r = client.post(
        f"/api/family/bridges/{bridge.id}/inspect", json={"link": link}, headers=world.admin_headers
    )
    assert r.status_code == 400
    assert r.json()["detail"] == "whatsapp links cannot be inspected; wait for a message"
    assert bridge_outbox.list_pending(bridge.id) == []


def test_inspect_by_google_link_still_works(client: TestClient, world: World):
    bridge, _ = pair_bridge(client, world, caps=WA_CAPS)
    r = client.post(
        f"/api/family/bridges/{bridge.id}/inspect",
        json={"link": "https://chat.google.com/room/AAA"},
        headers=world.admin_headers,
    )
    assert r.status_code == 202


# --- B11b: approval records the channel from the held row ------------------------


def _held_then_approved(client, world, broker, headers, event, name="Mom"):
    assert _post(client, headers, event) == ["held"]
    (alert,) = alerts_store.list_alerts(world.family_id, "open")
    r = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": name}, headers=world.admin_headers
    )
    assert r.status_code == 200, r.text
    (ext,) = externals_store.list_family_contacts(world.family_id)
    return ext


def test_approving_a_held_voice_text_records_via_and_thread(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    bridge, headers = pair_bridge(client, world, voice=VOICE, caps=WA_CAPS)
    ev = _dm(conv="voice-thread-9")
    ev["source"] = "gvoice"
    ext = _held_then_approved(client, world, broker, headers, ev)
    cfg = _sms_config(ext.uid)
    assert cfg["via"] == {"kid": "gvoice"} and cfg["voiceConv"] == {"kid": "voice-thread-9"}
    _send(broker, ext, "u_ap2")
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.source == "gvoice" and item.to["conversationId"] == "voice-thread-9"


def test_approving_a_held_row_without_via_changes_nothing(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world, voice=VOICE, caps=WA_CAPS)
    ev = _dm(conv="voice-thread-9")
    ev["source"] = "gvoice"
    assert _post(client, headers, ev) == ["held"]
    for snap in get_db().collection("heldSms").stream():  # a pre-B11b row
        snap.reference.update({"via": None, "conv": None})
    (alert,) = alerts_store.list_alerts(world.family_id, "open")
    r = client.post(f"/api/family/alerts/{alert.id}/approve", json={"name": "Mom"}, headers=world.admin_headers)
    assert r.status_code == 200
    (ext,) = externals_store.list_family_contacts(world.family_id)
    assert "via" not in _sms_config(ext.uid)

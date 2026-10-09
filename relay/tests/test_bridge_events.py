"""`POST /bridge/events` for `sms` and `gvoice` -- docs/BRIDGE_PHONE_DESIGN.md
decision 5 and `app/inbound_text.py`."""

from __future__ import annotations

import logging

from fastapi.testclient import TestClient

from app.db.firestore import get_db
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import externals as externals_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.bridge_world import (  # noqa: F401
    MOM,
    SIM,
    VOICE,
    RecordingFCM,
    World,
    broker,
    client,
    fcm,
    make_pager_device,
    pair_bridge,
    world,
)
from tests.fake_transport import FakeBrokerClient

STRANGER = "+12065550222"
_n = 0


def _ev(source: str = "sms", *, phone: str = MOM, text: str = "hi", conv: str = "c1", **extra):
    global _n
    _n += 1
    return {
        "id": f"e{_n}",
        "source": source,
        "conversation": {"id": conv, "isGroup": False},
        "sender": {"name": "Mom", "phone": phone},
        "text": text,
        "ts": 1,
        **extra,
    }


def _post(client: TestClient, headers, *events):
    r = client.post("/bridge/events", json={"events": list(events)}, headers=headers)
    assert r.status_code == 200, r.text
    return [x["outcome"] for x in r.json()["results"]]


def _kid_setup(world: World, broker: FakeBrokerClient):
    get_db().collection("users").document("kid").update(
        {"policy": {"out": "people_sms", "in": "people_sms"}}
    )
    make_pager_device("pgr-ev", "kid")
    backends_store.create_backend("kid", kind="pager", config={"deviceId": "pgr-ev"}, enabled=True)


def _mom(world: World, phone: str = MOM, members=("kid",)):
    ext = externals_store.get_or_create(world.family_id, phone, "Mom")
    for m in members:
        allow_store.set_edge(m, ext.uid, message=True, locate=False)
    return ext


def _inbox(uid: str = "kid"):
    return [m for m in get_db().collection("messages").stream() if m.to_dict()["recipientUid"] == uid]


def _sms_config(ext_uid: str) -> dict:
    return backends_store.get_backend(ext_uid, "sms").config


def test_known_contact_sms_reaches_the_pager_and_records_via(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)
    ext = _mom(world)
    assert _post(client, headers, _ev(text="hello kid")) == ["delivered"]
    (m,) = _inbox()
    assert m.to_dict()["body"] == "hello kid" and m.to_dict()["senderUid"] == ext.uid
    assert any(p.topic == "pager/pgr-ev/down" for p in broker.published)
    assert _sms_config(ext.uid)["via"] == {"kid": "sms"}


def test_gvoice_is_the_same_contact_and_switches_the_reply_channel(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    from app.routing import Routing

    _kid_setup(world, broker)
    bridge, headers = pair_bridge(client, world, sim=SIM, voice=VOICE)
    ext = _mom(world)
    assert _post(client, headers, _ev("sms"), _ev("gvoice", conv="voice-thread-1")) == [
        "delivered",
        "delivered",
    ]
    cfg = _sms_config(ext.uid)
    assert cfg["via"] == {"kid": "gvoice"} and cfg["voiceConv"] == {"kid": "voice-thread-1"}
    assert len(externals_store.list_family_contacts(world.family_id)) == 1
    Routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="reply",
        origin_backend_kind="pager", wire_id="u_1",
    )
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.source == "gvoice" and item.to["conversationId"] == "voice-thread-1"
    assert item.to["phone"] == MOM and item.to["link"].endswith(f"t.{MOM}")


def test_a_siblings_sim_text_leaves_the_first_members_via_alone(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    users_store.create_user(uid="sis", alias="sis", display_name="Sis", family_id=world.family_id)
    get_db().collection("users").document("sis").update(
        {"policy": {"out": "people_sms", "in": "people_sms"}}
    )
    _, kid_headers = pair_bridge(client, world, sim=SIM, voice=VOICE)
    _, sis_headers = pair_bridge(client, world, owner="sis", sim="+12065550888")
    ext = _mom(world, members=("kid", "sis"))
    _post(client, kid_headers, _ev("gvoice", conv="vt"))
    assert _post(client, sis_headers, _ev("sms")) == ["delivered"]
    assert _sms_config(ext.uid)["via"] == {"kid": "gvoice", "sis": "sms"}


def test_group_event_is_dropped(client: TestClient, world: World, broker: FakeBrokerClient):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)
    ev = _ev()
    ev["conversation"]["isGroup"] = True
    assert _post(client, headers, ev) == ["dropped_group"]
    assert _inbox() == []


def test_drop_outcomes(client: TestClient, world: World, broker: FakeBrokerClient):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)  # SIM only: no gvoice cap
    assert _post(client, headers, _ev(phone="nope")) == ["dropped_bad_from"]
    no_phone = _ev()
    del no_phone["sender"]["phone"]
    assert _post(client, headers, no_phone) == ["dropped_bad_from"]
    assert _post(client, headers, _ev("gvoice")) == ["dropped_cap"]
    inspect = _ev(kind="inspect")
    assert _post(client, headers, inspect) == ["dropped_unsupported"]
    users_store.update_user("kid", disabled=True)
    assert _post(client, headers, _ev()) == ["dropped_owner"]


def test_unknown_number_is_held_with_an_alert_whose_push_carries_the_text(
    client: TestClient, world: World, broker: FakeBrokerClient, fcm: RecordingFCM
):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)
    assert _post(client, headers, _ev(phone=STRANGER, text="buy gold now")) == ["held"]
    (alert,) = alerts_store.list_alerts(world.family_id, "open")
    assert alert.kind == "sms_unknown"
    assert any("buy gold now" in str(call[1]) for call in fcm.calls)
    assert _inbox() == []
    (held,) = get_db().collection("heldSms").stream()
    assert held.id.startswith("br_")


def test_duplicate_event_id_is_a_duplicate(client: TestClient, world: World, broker: FakeBrokerClient):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)
    _mom(world)
    ev = _ev()
    assert _post(client, headers, ev) == ["delivered"]
    assert _post(client, headers, ev) == ["duplicate"]
    assert len(_inbox()) == 1
    stranger = _ev(phone=STRANGER)
    assert _post(client, headers, stranger) == ["held"]
    assert _post(client, headers, stranger) == ["duplicate"]


def test_attachments_only_text_becomes_a_placeholder(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)
    _mom(world)
    assert _post(client, headers, _ev(text="", attachments=[{"kind": "image"}])) == ["delivered"]
    assert _post(
        client, headers, _ev(text="", attachments=[{"kind": "image"}, {"kind": "file"}])
    ) == ["delivered"]
    assert _post(client, headers, _ev(text="")) == ["dropped_empty"]
    assert sorted(m.to_dict()["body"] for m in _inbox()) == ["[attachment]", "[photo]"]


def test_too_long_enqueues_the_hint_once_even_when_the_batch_repeats(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    _kid_setup(world, broker)
    bridge, headers = pair_bridge(client, world)
    _mom(world)
    ev = _ev(text="x" * 200)
    assert _post(client, headers, ev) == ["too_long"]
    assert _post(client, headers, ev) == ["too_long"]
    (item,) = bridge_outbox.list_pending(bridge.id)
    assert item.id == f"ob_h_br_{bridge.id}_{ev['id']}" and item.msgId is None
    assert item.to == {"phone": MOM} and "too long" in item.text
    assert _inbox() == [] and list(get_db().collection("heldSms").stream()) == []


def test_rate_limit_121_events_in_a_minute_is_429(client: TestClient, world: World, broker: FakeBrokerClient):
    _kid_setup(world, broker)
    _, headers = pair_bridge(client, world)
    for _ in range(2):
        assert _post(client, headers, *[_ev(text="") for _ in range(50)])
    assert _post(client, headers, *[_ev(text="") for _ in range(20)])
    r = client.post("/bridge/events", json={"events": [_ev(text="")]}, headers=headers)
    assert r.status_code == 429


def test_validation_bounds_are_422(client: TestClient, world: World):
    _, headers = pair_bridge(client, world)
    assert client.post("/bridge/events", json={"events": [_ev(text="x" * 1601)]}, headers=headers).status_code == 422
    bad_id = _ev()
    bad_id["id"] = "has space"
    assert client.post("/bridge/events", json={"events": [bad_id]}, headers=headers).status_code == 422
    long_conv = _ev(conv="é" * 300)
    assert client.post("/bridge/events", json={"events": [long_conv]}, headers=headers).status_code == 422
    too_many = [_ev(text="") for _ in range(51)]
    assert client.post("/bridge/events", json={"events": too_many}, headers=headers).status_code == 422
    assert client.post("/bridge/events", json={"events": [_ev()]}).status_code == 401


def test_log_line_redacts_the_number(
    client: TestClient, world: World, broker: FakeBrokerClient, caplog
):
    caplog.set_level(logging.INFO, logger="relay.bridge")
    _kid_setup(world, broker)
    bridge, headers = pair_bridge(client, world)
    _mom(world)
    _post(client, headers, _ev())
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("bridge in "))
    assert line.startswith(f"bridge in bridge={bridge.id} src=sms conv=")
    assert "from=...0111 outcome=delivered" in line and MOM not in line
    assert messages_store is not None and bridges_store is not None

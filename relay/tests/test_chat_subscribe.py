"""Subscribe / ignore / patch / unsubscribe, routing and policy for a bridged
Google Chat conversation -- docs/BRIDGE_PHONE_DESIGN.md decisions 7, 9, 10."""

from __future__ import annotations

import json
import time

import pytest
from fastapi.testclient import TestClient

from app import chat_subscribe
from app.db.firestore import get_db
from app.ingest import Ingest
from app.routing import Routing
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import bridge_conversations as conv_store
from app.store import bridge_outbox
from app.store import bridges as bridges_store
from app.store import conversations as conversations_store
from app.store import externals as externals_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.bridge_world import (  # noqa: F401
    RecordingFCM,
    World,
    broker,
    client,
    fcm,
    make_pager_device,
    pair_bridge,
    world,
)
from tests.conftest import up_topic
from tests.fake_transport import FakeBrokerClient

GROUP = "spaces/AAA|soccer"
DM = "spaces/BBB|grandma"
LINK = "https://chat.google.com/room/AAA"
_n = 0


def _ev(sender: str, text: str, conv: str = GROUP, *, group: bool = True, title: str = "Soccer carpool", **extra):
    global _n
    _n += 1
    return {
        "id": f"s{_n}",
        "source": "gchat",
        "conversation": {"id": conv, "title": title, "isGroup": group, "link": LINK},
        "sender": {"name": sender},
        "text": text,
        "ts": 1,
        **extra,
    }


def _post(client: TestClient, headers, *events) -> list[str]:
    r = client.post("/bridge/events", json={"events": list(events)}, headers=headers)
    assert r.status_code == 200, r.text
    return [x["outcome"] for x in r.json()["results"]]


class Env:
    def __init__(self, client: TestClient, world: World, broker: FakeBrokerClient) -> None:
        self.client = client
        self.world = world
        self.broker = broker
        make_pager_device("pgr-chat", "kid")
        backends_store.create_backend("kid", kind="pager", config={"deviceId": "pgr-chat"}, enabled=True)
        self.bridge, self.headers = pair_bridge(client, world)

    def events(self, *events) -> list[str]:
        return _post(self.client, self.headers, *events)

    def sub(self, conv: str, body: dict, *, status: int = 200):
        ref = conv_store.conv_ref(conv)
        r = self.client.post(
            f"/api/family/bridges/{self.bridge.id}/conversations/{ref}/subscribe",
            json=body,
            headers=self.world.admin_headers,
        )
        assert r.status_code == status, r.text
        return r.json()

    def downs(self) -> list[dict]:
        return [
            json.loads(p.payload)
            for p in self.broker.published
            if p.topic == "pager/pgr-chat/down"
        ]

    def pager_up(self, to: str, body: str, msg_id: str) -> list[dict]:
        before = len(self.broker.published)
        payload = json.dumps(
            {"v": 1, "id": msg_id, "ts": int(time.time()), "from": "kid", "to": to, "body": body, "ack": None}
        ).encode()
        Ingest(self.broker, Routing(self.broker)).handle_up(up_topic("pgr-chat"), payload)
        return [json.loads(p.payload) for p in self.broker.published[before:]]


@pytest.fixture
def env(client: TestClient, world: World, broker: FakeBrokerClient) -> Env:
    return Env(client, world, broker)


ROSTER = [{"name": "Dana P", "nick": "dana"}, {"name": "Lee", "nick": "lee"}]


def _group_subscribe(env: Env, **over) -> dict:
    env.events(_ev("Dana P", "who drives?"), _ev("Lee", "me"))
    body = {"pagerName": "Soccer", "canReply": True, "roster": ROSTER, **over}
    return env.sub(GROUP, body)


def test_subscribe_group_creates_external_group_edge_and_delivers_backlog_in_order(env: Env):
    out = _group_subscribe(env)
    assert out["delivered"] == 2 and out["undelivered"] == 0
    ext_uid, ext_alias, _ = externals_store.chat_ids(env.bridge.id, GROUP)
    assert out["uid"] == ext_uid and out["alias"].startswith("bc") and len(out["alias"]) == 10
    ext = users_store.get_user(ext_uid)
    assert ext.kind == "external" and ext.phone is None and ext.displayName == "Soccer"
    assert ext.chat["bridgeId"] == env.bridge.id and ext.chat["canReply"] is True
    assert ext.alias == ext_alias and len(ext_alias) == 12
    group = conversations_store.get_by_alias(out["alias"])
    assert group.uids == sorted(["kid", ext_uid]) and group.bridge["conversationId"] == GROUP
    assert group.roster == {"dana": "Dana P", "lee": "Lee"} and group.convKey == out["convKey"]
    assert allow_store.is_message_allowed("kid", ext_uid)
    (row_backend,) = backends_store.list_backends(ext_uid)
    assert row_backend.kind == "bridge" and row_backend.id == "bridge"
    # The backlog reached the pager as group pages, oldest first, sndr = the nick.
    pages = [d for d in env.downs() if d.get("kind") != "book" and "body" in d]
    assert [(p["from"], p["sndr"], p["body"]) for p in pages] == [
        (out["alias"], "dana", "who drives?"),
        (out["alias"], "lee", "me"),
    ]
    # Book bump: a `book` nudge went to the device.
    assert any(d.get("kind") == "book" for d in env.downs())
    (alert,) = alerts_store.list_alerts(env.world.family_id, "all")
    assert alert.status == "handled"
    row = conv_store.get(env.bridge.id, GROUP)
    assert row.status == "subscribed" and row.uid == ext_uid and row.pagerName == "Soccer"
    assert row.customName is True  # "Soccer" differs from the title
    assert out["conversation"]["status"] == "subscribed"


def test_default_pager_name_is_not_custom(env: Env):
    env.events(_ev("Dana P", "hi"))
    env.sub(GROUP, {"pagerName": "Soccer carpool", "roster": ROSTER[:1]})
    assert conv_store.get(env.bridge.id, GROUP).customName is False


def test_subscribe_dm_creates_only_the_external_and_a_chat_book_entry(env: Env):
    env.events(_ev("Grandma", "hello kid", DM, group=False, title="Grandma"))
    out = env.sub(DM, {"pagerName": "Grandma"})
    assert out["convKey"] is None and out["delivered"] == 1
    ext_uid, ext_alias, _ = externals_store.chat_ids(env.bridge.id, DM)
    assert out["alias"] == ext_alias
    assert conversations_store.list_groups_for_member("kid") == []
    (page,) = [d for d in env.downs() if "body" in d]
    assert page["from"] == ext_alias and "sndr" not in page and page["body"] == "hello kid"
    r = env.client.get("/api/book", headers=env.world.admin_headers, params={"uid": "kid"})
    (entry,) = [e for e in r.json()["entries"] if e["uid"] == ext_uid]
    assert entry["chat"] == {"source": "gchat"} and entry["sendable"] and entry["onPager"]
    from app import devcfg

    contacts = devcfg._approved_contacts("kid")
    assert {"a": ext_alias, "n": "Grandma", "t": "chat"} in contacts


def test_group_book_entry_is_t_grp_and_the_external_is_not_listed_twice(env: Env):
    out = _group_subscribe(env)
    from app import devcfg

    contacts = devcfg._approved_contacts("kid")
    assert [c for c in contacts if c["a"] == out["alias"]] == [
        {"a": out["alias"], "n": "Soccer", "t": "grp"}
    ]
    ext_alias = externals_store.chat_ids(env.bridge.id, GROUP)[1]
    assert not any(c["a"] == ext_alias for c in contacts)


def test_pager_reply_to_group_alias_lands_in_the_outbox_with_conversation_and_link(env: Env):
    out = _group_subscribe(env)
    replies = env.pager_up(out["alias"], "I can drive", "u_g1")
    assert replies == []  # no system reply
    (item,) = bridge_outbox.list_pending(env.bridge.id)
    assert item.to == {"conversationId": GROUP, "link": LINK, "title": "Soccer carpool"}
    assert item.text == "I can drive"
    assert item.source == "gchat" and item.replyHint == GROUP
    (msg,) = [m for m in get_db().collection("messages").stream() if m.to_dict()["senderUid"] == "kid"]
    assert msg.to_dict()["deliveries"]["bridge"]["state"] == "queued"


def test_pager_reply_to_a_dm_chat_alias(env: Env):
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    out = env.sub(DM, {"pagerName": "Grandma"})
    env.pager_up(out["alias"], "hi gran", "u_d1")
    (item,) = bridge_outbox.list_pending(env.bridge.id)
    assert item.to["conversationId"] == DM and item.text == "hi gran"


def test_can_reply_false_rejects_owner_outbound_but_inbound_still_works(env: Env):
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    out = env.sub(DM, {"pagerName": "Grandma", "canReply": False})
    result = Routing(env.broker).send(
        sender_uid="kid", recipient_alias=out["alias"], kind="text", body="x", origin_backend_kind="pager"
    )
    assert [r.reason for r in result.rejected] == ["not_allowed"]
    # Default `people` inbound policy still lets the conversation reach kid.
    assert env.events(_ev("Grandma", "still there?", DM, group=False, title="Grandma")) == ["delivered"]
    assert env.downs()[-1]["body"] == "still there?"
    # ... and it is not offered in the pager's book.
    r = env.client.get("/api/book", headers=env.world.admin_headers, params={"uid": "kid"})
    (entry,) = [e for e in r.json()["entries"] if e["alias"] == out["alias"]]
    assert entry["sendable"] is False and entry["reason"] == "not_allowed"


def test_out_policy_none_forces_read_only_without_409(env: Env):
    get_db().collection("users").document("kid").update({"policy": {"out": "sms", "in": "people"}})
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    out = env.sub(DM, {"pagerName": "Grandma", "canReply": True})
    assert users_store.get_user(out["uid"]).chat["canReply"] is False


def test_in_policy_allowing_nobody_is_409(env: Env):
    get_db().collection("users").document("kid").update({"policy": {"out": "people", "in": "sms"}})
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    r = env.sub(DM, {"pagerName": "Grandma"}, status=409)
    assert "inbound policy" in r["detail"]


def test_paused_drops_inbound_and_fails_outbound(env: Env):
    out = _group_subscribe(env)
    ref = conv_store.conv_ref(GROUP)
    url = f"/api/family/bridges/{env.bridge.id}/conversations/{ref}"
    r = env.client.patch(url, json={"paused": True}, headers=env.world.admin_headers)
    assert r.status_code == 200 and r.json()["status"] == "paused"
    assert env.events(_ev("Lee", "ping")) == ["dropped_paused"]
    env.pager_up(out["alias"], "hello?", "u_p1")
    assert bridge_outbox.list_pending(env.bridge.id) == []
    (msg,) = [m for m in get_db().collection("messages").stream() if m.to_dict()["senderUid"] == "kid"]
    delivery = msg.to_dict()["deliveries"]["bridge"]
    assert delivery["state"] == "failed" and delivery["error"] == "paused"
    r = env.client.patch(url, json={"paused": False}, headers=env.world.admin_headers)
    assert r.json()["status"] == "subscribed"
    assert env.events(_ev("Lee", "back")) == ["delivered"]


def test_live_event_after_subscribe_is_delivered_with_roster_nick_and_auto_roster(env: Env):
    out = _group_subscribe(env)
    assert env.events(_ev("Dana P", "five minutes")) == ["delivered"]
    assert env.downs()[-1]["sndr"] == "dana"
    # A speaker not in the roster gets a slug nick, added to the roster.
    assert env.events(_ev("Sam Q", "hi all")) == ["delivered"]
    assert env.downs()[-1]["sndr"] == "sam-q"
    group = conversations_store.get_by_alias(out["alias"])
    assert group.roster["sam-q"] == "Sam Q"
    # A roster-less sender with a non-ASCII name gets a valid p... nick.
    assert env.events(_ev("奶奶", "hello")) == ["delivered"]
    nick = env.downs()[-1]["sndr"]
    assert nick.startswith("p") and len(nick) == 7
    from app.wire import is_valid_alias

    assert is_valid_alias(nick)
    # A retried event is a duplicate.
    again = _ev("Dana P", "dup")
    assert env.events(again) == ["delivered"]
    assert env.events(again) == ["duplicate"]


def test_long_subscribed_text_is_too_long_and_kept_not_on_the_pager(env: Env):
    _group_subscribe(env)
    pages_before = len(env.downs())
    assert env.events(_ev("Lee", "x" * 200)) == ["too_long"]
    assert len(env.downs()) == pages_before
    held = [h for h in get_db().collection("heldChat").stream() if h.to_dict()["status"] == "too_long"]
    assert len(held) == 1


def test_title_change_renames_unless_custom(env: Env):
    out = _group_subscribe(env, pagerName="Soccer carpool")
    assert env.events(_ev("Lee", "x", title="Soccer 2027")) == ["delivered"]
    assert users_store.get_user(out["uid"]).displayName == "Soccer 2027"
    assert conversations_store.get_by_alias(out["alias"]).name == "Soccer 2027"
    ref = conv_store.conv_ref(GROUP)
    env.client.patch(
        f"/api/family/bridges/{env.bridge.id}/conversations/{ref}",
        json={"pagerName": "My team"},
        headers=env.world.admin_headers,
    )
    assert env.events(_ev("Lee", "y", title="Soccer 2028")) == ["delivered"]
    assert users_store.get_user(out["uid"]).displayName == "My team"
    # A parent's rename locks the title: the phone no longer rewrites it.
    assert conv_store.get(env.bridge.id, GROUP).title == "Soccer 2027"


def test_unsubscribe_removes_docs_and_edges_and_keeps_messages(env: Env):
    out = _group_subscribe(env)
    ref = conv_store.conv_ref(GROUP)
    r = env.client.delete(
        f"/api/family/bridges/{env.bridge.id}/conversations/{ref}", headers=env.world.admin_headers
    )
    assert r.status_code == 200 and r.json()["status"] == "seen" and r.json()["uid"] is None
    assert users_store.get_user(out["uid"]) is None
    assert conversations_store.get_by_alias(out["alias"]) is None
    assert not allow_store.is_message_allowed("kid", out["uid"])
    assert len(list(get_db().collection("messages").stream())) == 2
    assert users_store.get_uid_for_alias(externals_store.chat_ids(env.bridge.id, GROUP)[1]) is None
    # A later text raises a fresh alert; the same name can be reused.
    assert env.events(_ev("Lee", "again")) == ["held"]
    env.sub(GROUP, {"pagerName": "Soccer", "roster": ROSTER})


def test_subscribe_twice_is_idempotent(env: Env):
    first = _group_subscribe(env)
    again = env.sub(GROUP, {"pagerName": "Soccer", "roster": ROSTER})
    assert again["uid"] == first["uid"] and again["alias"] == first["alias"]
    assert again["delivered"] == 0
    assert len([d for d in env.downs() if "sndr" in d]) == 2
    assert len(list(conversations_store._conversations().where("kind", "==", "group").stream())) == 1
    assert len(externals_store.list_family_contacts(env.world.family_id)) == 1


def test_validation_errors(env: Env):
    env.events(_ev("Dana P", "hi"))
    env.sub(GROUP, {"pagerName": "Soccer", "roster": [{"name": "Dana P", "nick": "Dana P"}]}, status=422)
    env.sub(GROUP, {"pagerName": "Soccer", "roster": [{"name": "A", "nick": "x"}, {"name": "B", "nick": "x"}]}, status=422)
    env.sub(GROUP, {"pagerName": "x" * 17, "roster": []}, status=422)
    env.sub(GROUP, {"pagerName": "  ", "roster": []}, status=422)
    users_store.create_user(uid="mom", alias="mom", display_name="Soccer", family_id=env.world.family_id)
    env.sub(GROUP, {"pagerName": "soccer", "roster": []}, status=409)
    # Unknown ref / bridge.
    r = env.client.post(
        f"/api/family/bridges/{env.bridge.id}/conversations/nope/subscribe",
        json={"pagerName": "Z"},
        headers=env.world.admin_headers,
    )
    assert r.status_code == 404
    assert conv_store.get(env.bridge.id, GROUP).status == "seen"


def test_ignore_drops_later_events_and_dismisses_alert(env: Env):
    env.events(_ev("Dana P", "hi"))
    ref = conv_store.conv_ref(GROUP)
    r = env.client.post(
        f"/api/family/bridges/{env.bridge.id}/conversations/{ref}/ignore", headers=env.world.admin_headers
    )
    assert r.status_code == 200 and r.json()["status"] == "ignored"
    (alert,) = alerts_store.list_alerts(env.world.family_id, "all")
    assert alert.status == "dismissed"
    assert env.events(_ev("Dana P", "again")) == ["dropped_ignored"]
    # Subscribing an ignored conversation is allowed (it brings it back).
    env.sub(GROUP, {"pagerName": "Soccer", "roster": []})
    assert conv_store.get(env.bridge.id, GROUP).status == "subscribed"


def test_no_bridge_system_reply_when_unpaired_for_dm_and_group(env: Env):
    out_g = _group_subscribe(env)
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    out_d = env.sub(DM, {"pagerName": "Grandma"})
    bridges_store.unpair(env.bridge.id)
    for i, alias in enumerate((out_g["alias"], out_d["alias"])):
        replies = env.pager_up(alias, "hi", f"u_nb{i}")
        assert [r["body"] for r in replies] == ["bridge not set up; ask your admin"]
        assert replies[0]["from"] == "system"
    assert bridge_outbox.list_pending(env.bridge.id) == []


def test_sibling_cannot_join_or_post_into_a_bridge_group(env: Env):
    out = _group_subscribe(env)
    users_store.create_user(uid="sis", alias="sis", display_name="Sis", family_id=env.world.family_id)
    r = env.client.post(
        f"/api/conversations/{out['alias']}/members", json={"uid": "sis"}, headers=env.world.admin_headers
    )
    assert r.status_code == 409 and r.json()["detail"] == "managed under Google Chat"
    result = Routing(env.broker).send(
        sender_uid="sis", recipient_alias=out["alias"], kind="text", body="x", origin_backend_kind="pager"
    )
    assert [r.reason for r in result.rejected] == ["not_member"]


def test_sibling_with_open_policy_cannot_reach_a_chat_external(env: Env):
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    out = env.sub(DM, {"pagerName": "Grandma"})
    users_store.create_user(uid="sis", alias="sis", display_name="Sis", family_id=env.world.family_id)
    get_db().collection("users").document("sis").update({"policy": {"out": "open", "in": "any"}})
    allow_store.set_edge("sis", out["uid"], message=True, locate=False)
    result = Routing(env.broker).send(
        sender_uid="sis", recipient_alias=out["alias"], kind="text", body="x", origin_backend_kind="pager"
    )
    assert [r.reason for r in result.rejected] == ["no_bridge"]
    assert bridge_outbox.list_pending(env.bridge.id) == []


def test_put_approved_keeps_the_subscribe_edge_and_hides_the_external(env: Env):
    env.events(_ev("Grandma", "hello", DM, group=False, title="Grandma"))
    out = env.sub(DM, {"pagerName": "Grandma"})
    headers = env.world.admin_headers
    got = env.client.get("/api/family/members/kid/approved", headers=headers).json()
    assert got["contacts"] == []
    r = env.client.put(
        "/api/family/members/kid/approved", json={"people": [], "contacts": []}, headers=headers
    )
    assert r.status_code == 200
    assert allow_store.is_message_allowed("kid", out["uid"])
    r = env.client.put(
        "/api/family/members/kid/approved",
        json={"people": [], "contacts": [{"uid": out["uid"]}]},
        headers=headers,
    )
    assert r.status_code == 404
    assert env.client.get("/api/family/contacts", headers=headers).json() == []


def test_member_chat_tab_lists_subscribed_and_seen(env: Env):
    _group_subscribe(env)
    env.events(_ev("Pat", "hi", "spaces/CCC", group=False, title="Pat"))
    r = env.client.get("/api/family/members/kid/chat", headers=env.world.admin_headers)
    assert r.status_code == 200
    body = r.json()
    (sub,) = body["subscribed"]
    assert sub["pagerName"] == "Soccer" and sub["onPager"] is True and sub["canReply"] is True
    assert sub["roster"] == [{"name": "Dana P", "nick": "dana"}, {"name": "Lee", "nick": "lee"}]
    assert sub["ref"] == conv_store.conv_ref(GROUP) and sub["alias"].startswith("bc")
    (seen,) = body["seen"]
    assert seen["title"] == "Pat" and seen["status"] == "seen" and seen["heldCount"] == 1
    assert body["bridges"][0]["id"] == env.bridge.id and body["bridges"][0]["paired"] is True
    assert "tokenHash" not in json.dumps(body)


def test_inspect_by_link(env: Env):
    url = f"/api/family/bridges/{env.bridge.id}/inspect"
    r = env.client.post(url, json={"link": "https://chat.google.com/dm/xyz"}, headers=env.world.admin_headers)
    assert r.status_code == 202
    item = bridge_outbox.get(env.bridge.id, r.json()["outboxId"])
    assert item.kind == "inspect" and item.to == {"link": "https://chat.google.com/dm/xyz"}
    for good in ("https://mail.google.com/chat/u/0/#chat/dm/x", "https://voice.google.com/u/0/messages"):
        assert env.client.post(url, json={"link": good}, headers=env.world.admin_headers).status_code == 202
    for bad in ("http://chat.google.com/x", "https://evil.example.com/chat.google.com/", "javascript:1"):
        assert env.client.post(url, json={"link": bad}, headers=env.world.admin_headers).status_code == 422


def test_other_familys_bridge_is_404(env: Env):
    from firebase_admin import auth as fb_auth

    from app.store import families as families_store
    from tests.firebase_test_utils import auth_header

    other = families_store.create_family(name="G", created_by="root")
    fb_auth.create_user(uid="adm2", email="adm2@example.com")
    users_store.create_user(uid="adm2", alias="adm2", display_name="a2", role="admin", family_id=other.id)
    fb_auth.set_custom_user_claims("adm2", {"role": "admin", "fam": other.id})
    ref = conv_store.conv_ref(GROUP)
    env.events(_ev("Dana P", "hi"))
    r = env.client.post(
        f"/api/family/bridges/{env.bridge.id}/conversations/{ref}/ignore", headers=auth_header("adm2")
    )
    assert r.status_code == 404
    assert chat_subscribe.slug_nick("Grandma Jo", set()) == "grandma-jo"


def test_slug_nick_rules():
    assert chat_subscribe.slug_nick("Dana P.", set()) == "dana-p"
    assert chat_subscribe.slug_nick("Dana P.", {"dana-p"}) == "dana-p-2"
    assert chat_subscribe.slug_nick("Dana P.", {"dana-p", "dana-p-2"}) == "dana-p-3"
    long = chat_subscribe.slug_nick("A" * 30, {"a" * 16})
    assert long == "a" * 13 + "-2" and len(long) == 15
    assert chat_subscribe.slug_nick("奶奶", set()).startswith("p")
    assert chat_subscribe.slug_nick("", set()).startswith("p")
    assert chat_subscribe.slug_nick("system", set()) == "system-2"
    assert messages_store and bridge_outbox and bridges_store

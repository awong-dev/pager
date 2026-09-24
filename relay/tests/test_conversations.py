"""`/api/conversations/*` -- docs/SERVER_PLAN.md §5.1: send (allow-list
enforcement -> 403/404, success -> 201) and the read-receipt endpoint."""

from __future__ import annotations

import json
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "test-webhook-key",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture
def client() -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        yield c


def _make_user(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias)
    return auth_header(uid)


def _make_admin(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias, role="admin")
    fb_auth.set_custom_user_claims(uid, {"admin": True})
    return auth_header(uid)


def _make_pager_device(device_id: str, owner_uid: str) -> None:
    # Same helper `tests/test_contacts.py` uses for a `password`-auth pager
    # device -- no `deviceSecrets` row needed for `BrokerClient.publish_down`
    # to succeed against `FakeBrokerClient`.
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def test_send_message_success_returns_201_with_id(client: TestClient):
    mom_headers = _make_user("mom", "mom")
    _make_user("kid", "kid")
    allow_store.set_edge("mom", "kid", message=True, locate=True)

    resp = client.post(
        "/api/conversations/kid/messages", json={"body": "pickup at 3"}, headers=mom_headers
    )
    assert resp.status_code == 201, resp.text
    msg_id = resp.json()["id"]
    msg = messages_store.get_message(msg_id)
    assert msg is not None
    assert msg.senderUid == "mom"
    assert msg.recipientUid == "kid"
    assert msg.body == "pickup at 3"


def test_send_message_unknown_alias_is_404(client: TestClient):
    mom_headers = _make_user("mom2", "mom2")
    resp = client.post(
        "/api/conversations/ghost/messages", json={"body": "hi"}, headers=mom_headers
    )
    assert resp.status_code == 404


def test_send_message_disallowed_recipient_is_403(client: TestClient):
    mom_headers = _make_user("mom3", "mom3")
    _make_user("kid3", "kid3")
    # No allow edge.
    resp = client.post(
        "/api/conversations/kid3/messages", json={"body": "hi"}, headers=mom_headers
    )
    assert resp.status_code == 403


def test_send_message_empty_body_after_strip_is_400(client: TestClient):
    mom_headers = _make_user("mom4", "mom4")
    _make_user("kid4", "kid4")
    allow_store.set_edge("mom4", "kid4", message=True, locate=True)
    resp = client.post(
        "/api/conversations/kid4/messages", json={"body": "\x00\x01"}, headers=mom_headers
    )
    assert resp.status_code == 400


def test_send_message_requires_auth(client: TestClient):
    resp = client.post("/api/conversations/kid/messages", json={"body": "hi"})
    assert resp.status_code == 401


def test_mark_read_sets_webapp_delivery_to_read(client: TestClient):
    mom_headers = _make_user("mom5", "mom5")
    kid_headers = _make_user("kid5", "kid5")
    allow_store.set_edge("mom5", "kid5", message=True, locate=True)

    resp = client.post(
        "/api/conversations/kid5/messages", json={"body": "hi"}, headers=mom_headers
    )
    msg_id = resp.json()["id"]

    # {alias} names the *other* party of the conversation from the caller's
    # point of view -- kid5 (the recipient, marking a message read) posts to
    # the conversation with mom5, mirroring how mom5 posted to kid5 above.
    read_resp = client.post(
        f"/api/conversations/mom5/messages/{msg_id}/read", headers=kid_headers
    )
    assert read_resp.status_code == 200, read_resp.text

    msg = messages_store.get_message(msg_id)
    webapp_bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "webapp")
    assert msg.deliveries[webapp_bid].state == "read"


def test_mark_read_clears_conversation_unread_for_recipient(client: TestClient):
    mom_headers = _make_user("mom5b", "mom5b")
    kid_headers = _make_user("kid5b", "kid5b")
    allow_store.set_edge("mom5b", "kid5b", message=True, locate=True)

    resp = client.post(
        "/api/conversations/kid5b/messages", json={"body": "hi"}, headers=mom_headers
    )
    msg_id = resp.json()["id"]

    conv_key = messages_store.conv_key("mom5b", "kid5b")
    conv_before = messages_store.get_conversation(conv_key)
    assert conv_before is not None
    assert conv_before.unread["kid5b"] == 1

    read_resp = client.post(
        f"/api/conversations/mom5b/messages/{msg_id}/read", headers=kid_headers
    )
    assert read_resp.status_code == 200, read_resp.text

    conv_after = messages_store.get_conversation(conv_key)
    assert conv_after is not None
    assert conv_after.unread["kid5b"] == 0


def test_mark_read_mismatched_alias_is_404(client: TestClient):
    """S3b: the `{alias}` path segment must actually name this message's
    conversation -- posting the read against some *other* alias the caller
    is otherwise allowed to message must not silently succeed."""
    mom_headers = _make_user("mom5c", "mom5c")
    kid_headers = _make_user("kid5c", "kid5c")
    _make_user("other5c", "other5c")
    allow_store.set_edge("mom5c", "kid5c", message=True, locate=True)
    allow_store.set_edge("other5c", "kid5c", message=True, locate=True)

    resp = client.post(
        "/api/conversations/kid5c/messages", json={"body": "hi"}, headers=mom_headers
    )
    msg_id = resp.json()["id"]

    # kid5c is genuinely the recipient, but names the wrong conversation
    # (other5c instead of mom5c).
    read_resp = client.post(
        f"/api/conversations/other5c/messages/{msg_id}/read", headers=kid_headers
    )
    assert read_resp.status_code == 404

    msg = messages_store.get_message(msg_id)
    webapp_bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "webapp")
    assert msg.deliveries[webapp_bid].state != "read"


def test_mark_read_by_non_recipient_is_404(client: TestClient):
    mom_headers = _make_user("mom6", "mom6")
    _make_user("kid6", "kid6")
    stranger_headers = _make_user("stranger6", "stranger6")
    allow_store.set_edge("mom6", "kid6", message=True, locate=True)

    resp = client.post(
        "/api/conversations/kid6/messages", json={"body": "hi"}, headers=mom_headers
    )
    msg_id = resp.json()["id"]

    read_resp = client.post(
        f"/api/conversations/kid6/messages/{msg_id}/read", headers=stranger_headers
    )
    assert read_resp.status_code == 404


# ---------------------------------------------------------------------------
# POST /api/conversations/{alias}/locate -- docs/SERVER_PLAN.md §5.1, §5.6
# ---------------------------------------------------------------------------


def _make_pager_device(device_id: str, owner_uid: str):
    # S1.4: `auth_mode="password"` -- these /locate tests don't set up a
    # deviceSecrets row, and only need the loc_req down publish to succeed.
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def test_locate_success_returns_202_with_request_id(client: TestClient):
    mom_headers = _make_user("mom7", "mom7")
    _make_user("kid7", "kid7")
    allow_store.set_edge("mom7", "kid7", message=True, locate=True)
    _make_pager_device("pgr-conv-1", "kid7")

    resp = client.post("/api/conversations/kid7/locate", headers=mom_headers)
    assert resp.status_code == 202, resp.text
    body = resp.json()
    assert body["requestId"]
    assert body["cached"] is False


def test_locate_unknown_alias_is_404(client: TestClient):
    mom_headers = _make_user("mom8", "mom8")
    resp = client.post("/api/conversations/nobody/locate", headers=mom_headers)
    assert resp.status_code == 404


def test_locate_without_locate_permission_is_403(client: TestClient):
    mom_headers = _make_user("mom9", "mom9")
    _make_user("kid9", "kid9")
    # message allowed, locate explicitly denied.
    allow_store.set_edge("mom9", "kid9", message=True, locate=False)
    _make_pager_device("pgr-conv-2", "kid9")

    resp = client.post("/api/conversations/kid9/locate", headers=mom_headers)
    assert resp.status_code == 403


def test_locate_no_allow_edge_at_all_is_403(client: TestClient):
    mom_headers = _make_user("mom10", "mom10")
    _make_user("kid10", "kid10")

    resp = client.post("/api/conversations/kid10/locate", headers=mom_headers)
    assert resp.status_code == 403


def test_locate_target_with_no_device_is_409(client: TestClient):
    mom_headers = _make_user("mom11", "mom11")
    _make_user("kid11", "kid11")
    allow_store.set_edge("mom11", "kid11", message=True, locate=True)
    # kid11 owns no device at all.

    resp = client.post("/api/conversations/kid11/locate", headers=mom_headers)
    assert resp.status_code == 409


def test_locate_requires_auth(client: TestClient):
    resp = client.post("/api/conversations/kid7/locate")
    assert resp.status_code == 401


# ---------------------------------------------------------------------------
# Group admin API -- docs/GROUP_CHAT_DESIGN.md §3, task G2.
# ---------------------------------------------------------------------------


def test_create_group_requires_admin(client: TestClient):
    mom_headers = _make_user("gapi-mom", "gapi-mom")
    _make_user("gapi-kid", "gapi-kid")
    resp = client.post(
        "/api/conversations",
        json={"name": "Family", "alias": "gapi-fam", "memberUids": ["gapi-mom", "gapi-kid"]},
        headers=mom_headers,
    )
    assert resp.status_code == 403


def test_create_group_success_returns_conv_key_and_creates_allow_edges(client: TestClient):
    admin_headers = _make_admin("gapi-admin1", "gapi-admin1")
    _make_user("gapi-m1", "gapi-m1")
    _make_user("gapi-m2", "gapi-m2")

    resp = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gapi-fam1",
            "memberUids": ["gapi-m1", "gapi-m2"],
        },
        headers=admin_headers,
    )
    assert resp.status_code == 201, resp.text
    body = resp.json()
    assert body["alias"] == "gapi-fam1"
    assert body["convKey"].startswith("g_")

    conv = messages_store.get_conversation(body["convKey"])
    assert conv is not None
    assert conv.kind == "group"
    assert conv.uids == ["gapi-m1", "gapi-m2"]

    # Decision 2: creating a group auto-creates allow edges both ways
    # between every member pair.
    assert allow_store.is_message_allowed("gapi-m1", "gapi-m2")
    assert allow_store.is_message_allowed("gapi-m2", "gapi-m1")


def test_create_group_tolerates_creator_already_in_member_uids(client: TestClient):
    """The web client always includes the creating admin's own uid in
    `memberUids` -- the endpoint must dedupe, not reject."""
    admin_headers = _make_admin("gapi-admin2", "gapi-admin2")
    _make_user("gapi-m3", "gapi-m3")

    resp = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gapi-fam2",
            "memberUids": ["gapi-admin2", "gapi-m3", "gapi-admin2"],
        },
        headers=admin_headers,
    )
    assert resp.status_code == 201, resp.text
    conv = messages_store.get_conversation(resp.json()["convKey"])
    assert conv is not None
    assert conv.uids == ["gapi-admin2", "gapi-m3"]


def test_create_group_alias_collision_is_409(client: TestClient):
    admin_headers = _make_admin("gapi-admin3", "gapi-admin3")
    _make_user("gapi-m4", "gapi-m4")
    _make_user("gapi-m5", "gapi-m5")
    payload = {"name": "Family", "alias": "gapi-dupe", "memberUids": ["gapi-m4", "gapi-m5"]}
    first = client.post("/api/conversations", json=payload, headers=admin_headers)
    assert first.status_code == 201, first.text

    second = client.post("/api/conversations", json=payload, headers=admin_headers)
    assert second.status_code == 409


def test_create_group_unknown_member_uid_is_404(client: TestClient):
    admin_headers = _make_admin("gapi-admin4", "gapi-admin4")
    resp = client.post(
        "/api/conversations",
        json={"name": "Family", "alias": "gapi-fam4", "memberUids": ["gapi-admin4", "ghost-uid"]},
        headers=admin_headers,
    )
    assert resp.status_code == 404


def test_send_group_message_and_mark_read(client: TestClient):
    admin_headers = _make_admin("gapi-admin5", "gapi-admin5")
    kid_headers = _make_user("gapi-kid5", "gapi-kid5")
    _make_user("gapi-kid6", "gapi-kid6")

    created = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gapi-fam5",
            "memberUids": ["gapi-admin5", "gapi-kid5", "gapi-kid6"],
        },
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text

    send_resp = client.post(
        "/api/conversations/gapi-fam5/messages", json={"body": "hi family"}, headers=admin_headers
    )
    assert send_resp.status_code == 201, send_resp.text
    msg_id = send_resp.json()["id"]

    msg = messages_store.get_message(msg_id)
    assert msg is not None
    assert msg.senderAlias == "gapi-admin5"
    assert msg.groupMsgId is not None

    read_resp = client.post(
        f"/api/conversations/gapi-fam5/messages/{msg_id}/read", headers=kid_headers
    )
    assert read_resp.status_code == 200, read_resp.text


def test_send_group_message_by_non_member_is_403(client: TestClient):
    admin_headers = _make_admin("gapi-admin6", "gapi-admin6")
    _make_user("gapi-kid7", "gapi-kid7")
    outsider_headers = _make_user("gapi-outsider", "gapi-outsider")

    created = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gapi-fam6",
            "memberUids": ["gapi-admin6", "gapi-kid7"],
        },
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text

    resp = client.post(
        "/api/conversations/gapi-fam6/messages", json={"body": "hi"}, headers=outsider_headers
    )
    assert resp.status_code == 403


def test_add_member_then_leave(client: TestClient):
    admin_headers = _make_admin("gapi-admin7", "gapi-admin7")
    kid_headers = _make_user("gapi-kid8", "gapi-kid8")
    _make_user("gapi-newmem", "gapi-newmem")

    created = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gapi-fam7",
            "memberUids": ["gapi-admin7", "gapi-kid8"],
        },
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text
    conv_key = created.json()["convKey"]

    add_resp = client.post(
        "/api/conversations/gapi-fam7/members", json={"uid": "gapi-newmem"}, headers=admin_headers
    )
    assert add_resp.status_code == 200, add_resp.text
    assert "gapi-newmem" in add_resp.json()["uids"]
    assert allow_store.is_message_allowed("gapi-newmem", "gapi-admin7")
    assert allow_store.is_message_allowed("gapi-admin7", "gapi-newmem")

    leave_resp = client.delete("/api/conversations/gapi-fam7/members/me", headers=kid_headers)
    assert leave_resp.status_code == 200, leave_resp.text
    assert "gapi-kid8" not in leave_resp.json()["uids"]

    conv = messages_store.get_conversation(conv_key)
    assert conv is not None
    assert "gapi-kid8" not in conv.uids
    assert conversations_store.is_member(conv_key, "gapi-newmem")


def test_add_member_by_non_member_is_403(client: TestClient):
    admin_headers = _make_admin("gapi-admin8", "gapi-admin8")
    _make_user("gapi-kid9", "gapi-kid9")
    outsider_headers = _make_user("gapi-outsider2", "gapi-outsider2")
    _make_user("gapi-target", "gapi-target")

    created = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gapi-fam8",
            "memberUids": ["gapi-admin8", "gapi-kid9"],
        },
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text

    resp = client.post(
        "/api/conversations/gapi-fam8/members",
        json={"uid": "gapi-target"},
        headers=outsider_headers,
    )
    assert resp.status_code == 403


# ---------------------------------------------------------------------------
# book push on membership change -- build/bench-logs/group-acceptance.md's
# "Gap found": create/add-member/leave must bump `devices/{d}.bookVersion`
# and re-publish the book to any member's device, the same
# bump_book_version+push_book idiom `app/routers/admin.py`'s contact
# approve/reject already uses.
# ---------------------------------------------------------------------------


def test_create_group_pushes_book_to_pager_member_but_not_web_only_member(
    client: TestClient,
):
    admin_headers = _make_admin("gbk-admin1", "gbk-admin1")
    _make_user("gbk-webonly1", "gbk-webonly1")
    _make_pager_device("gbk-dev1", "gbk-admin1")

    resp = client.post(
        "/api/conversations",
        json={
            "name": "Family",
            "alias": "gbk-fam1",
            "memberUids": ["gbk-admin1", "gbk-webonly1"],
        },
        headers=admin_headers,
    )
    assert resp.status_code == 201, resp.text

    broker = client.app.state.broker
    pushes = [p for p in broker.published if p.topic == "pager/gbk-dev1/down"]
    assert len(pushes) == 1
    sent = json.loads(pushes[0].payload)
    assert sent["kind"] == "book"
    assert {"a": "gbk-fam1", "n": "Family", "t": "grp"} in sent["c"]

    # No device for gbk-webonly1 -- nothing to push to, and no other topic
    # was touched.
    assert all(p.topic == "pager/gbk-dev1/down" for p in broker.published)


def test_create_group_bumps_book_version(client: TestClient):
    from app.db.firestore import get_db

    admin_headers = _make_admin("gbk-admin2", "gbk-admin2")
    _make_pager_device("gbk-dev2", "gbk-admin2")
    _make_user("gbk-kid2", "gbk-kid2")

    before = int(
        (get_db().collection("devices").document("gbk-dev2").get().to_dict() or {}).get(
            "bookVersion", 0
        )
    )

    resp = client.post(
        "/api/conversations",
        json={"name": "Family", "alias": "gbk-fam2", "memberUids": ["gbk-admin2", "gbk-kid2"]},
        headers=admin_headers,
    )
    assert resp.status_code == 201, resp.text

    after = int(
        (get_db().collection("devices").document("gbk-dev2").get().to_dict() or {}).get(
            "bookVersion", 0
        )
    )
    assert after == before + 1


def test_add_member_pushes_book_to_new_pager_member(client: TestClient):
    admin_headers = _make_admin("gbk-admin3", "gbk-admin3")
    _make_user("gbk-kid3", "gbk-kid3")
    _make_user("gbk-newmem3", "gbk-newmem3")
    _make_pager_device("gbk-dev3", "gbk-newmem3")

    created = client.post(
        "/api/conversations",
        json={"name": "Family", "alias": "gbk-fam3", "memberUids": ["gbk-admin3", "gbk-kid3"]},
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text

    broker = client.app.state.broker
    broker.published.clear()

    add_resp = client.post(
        "/api/conversations/gbk-fam3/members",
        json={"uid": "gbk-newmem3"},
        headers=admin_headers,
    )
    assert add_resp.status_code == 200, add_resp.text

    pushes = [p for p in broker.published if p.topic == "pager/gbk-dev3/down"]
    assert len(pushes) == 1
    sent = json.loads(pushes[0].payload)
    assert sent["kind"] == "book"
    assert {"a": "gbk-fam3", "n": "Family", "t": "grp"} in sent["c"]


def test_leave_group_pushes_book_to_leaving_pager_member_only(client: TestClient):
    admin_headers = _make_admin("gbk-admin4", "gbk-admin4")
    kid_headers = _make_user("gbk-kid4", "gbk-kid4")
    _make_pager_device("gbk-dev4-admin", "gbk-admin4")
    _make_pager_device("gbk-dev4-kid", "gbk-kid4")

    created = client.post(
        "/api/conversations",
        json={"name": "Family", "alias": "gbk-fam4", "memberUids": ["gbk-admin4", "gbk-kid4"]},
        headers=admin_headers,
    )
    assert created.status_code == 201, created.text

    broker = client.app.state.broker
    broker.published.clear()

    leave_resp = client.delete("/api/conversations/gbk-fam4/members/me", headers=kid_headers)
    assert leave_resp.status_code == 200, leave_resp.text

    # Only the leaver's device is pushed to -- the remaining member's book
    # content (their own group memberships) didn't change.
    topics = {p.topic for p in broker.published}
    assert topics == {"pager/gbk-dev4-kid/down"}
    sent = json.loads(broker.published[0].payload)
    assert sent["kind"] == "book"
    assert all(c["a"] != "gbk-fam4" for c in sent["c"])

"""`/api/conversations/*` -- docs/SERVER_PLAN.md §5.1: send (allow-list
enforcement -> 403/404, success -> 201) and the read-receipt endpoint."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
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
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
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

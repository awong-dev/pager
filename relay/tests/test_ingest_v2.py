"""`app.ingest.Ingest` against the v2 uid-addressed model (registered
`devices/{id}` docs) -- the drop+system-reply path (PROTOCOL.md §4.2 case 3),
v2 ack routing (find the right pager delivery, monotonic state), and the v2
online-edge republish (`pendingDeviceIds`). The legacy-model behaviour this
same class implements is already covered by tests/test_ingest.py; this file
only exercises the *new* branch (`devices/{device_id}` registered).
"""

from __future__ import annotations

import json
import time

from app.ingest import Ingest
from app.routing import Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.conftest import (
    ack_payload,
    online_status_payload,
    status_topic,
    up_message_payload,
    up_topic,
)
from tests.fake_transport import FakeBrokerClient


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


def _make_pager_device(device_id: str, owner_uid: str, *, default_to_uid: str | None = None):
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        default_to_uid=default_to_uid,
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def _ingest() -> tuple[Ingest, FakeBrokerClient]:
    broker = FakeBrokerClient()
    routing = Routing(broker)
    return Ingest(broker, routing), broker


# ---- drop + system reply (PROTOCOL.md §4.2 case 3) ----


def test_unknown_to_alias_gets_one_system_reply_and_is_not_stored():
    _make_user("student", "student")
    _make_pager_device("pgr-v2-1", "student")

    ingest, broker = _ingest()
    ingest.handle_up(
        up_topic("pgr-v2-1"),
        json.dumps(
            {
                "v": 1,
                "id": "u_unknown1",
                "ts": int(time.time()),
                "from": "student",
                "to": "stranger",
                "body": "hi",
                "ack": None,
            }
        ).encode("utf-8"),
    )

    assert len(broker.published) == 1
    sent = json.loads(broker.published[0].payload)
    assert sent["from"] == "system"
    assert sent["body"] == "unknown recipient"

    # The dropped up-message was never stored as a message doc for anyone.
    key = messages_store.conv_key("student", "stranger")
    assert messages_store.get_conversation(key) is None


def test_disallowed_to_alias_gets_system_reply_and_security_drop():
    _make_user("student", "student")
    _make_user("stranger2", "stranger2")
    _make_pager_device("pgr-v2-2", "student")
    # Deliberately no allow edge student->stranger2.

    ingest, broker = _ingest()
    ingest.handle_up(
        up_topic("pgr-v2-2"),
        json.dumps(
            {
                "v": 1,
                "id": "u_unknown2",
                "ts": int(time.time()),
                "from": "student",
                "to": "stranger2",
                "body": "hi",
                "ack": None,
            }
        ).encode("utf-8"),
    )
    assert len(broker.published) == 1
    sent = json.loads(broker.published[0].payload)
    assert sent["from"] == "system"
    assert sent["body"] == "unknown recipient"


def test_no_to_and_no_allowed_contacts_gets_no_system_reply():
    # §4.2 case 1 (no `to`) has no error path -- an empty broadcast set is
    # silently a no-op, never a system reply.
    _make_user("student", "student")
    _make_pager_device("pgr-v2-3", "student")

    ingest, broker = _ingest()
    ingest.handle_up(up_topic("pgr-v2-3"), up_message_payload("u_broadcast0", "hi"))
    assert broker.published == []


# ---- v2 ack routing ----


def test_v2_ack_updates_the_right_pager_delivery():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-v2-4", "student")

    ingest, broker = _ingest()
    routing = ingest._routing
    result = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]
    assert len(broker.published) == 1

    ingest.handle_up(up_topic("pgr-v2-4"), ack_payload(msg.id, "shown", ts=1_700_000_100))
    refreshed = messages_store.get_message(msg.id)
    pager_bid = next(bid for bid, d in refreshed.deliveries.items() if d.kind == "pager")
    assert refreshed.deliveries[pager_bid].state == "shown"
    assert refreshed.deliveries[pager_bid].shownTs == 1_700_000_100
    # Acked -> no longer pending.
    assert "pgr-v2-4" not in refreshed.pendingDeviceIds


def test_v2_ack_wrong_device_is_dropped():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-v2-5a", "student")
    _make_pager_device("pgr-v2-5b", "mom")  # unrelated device owned by someone else

    ingest, _broker = _ingest()
    routing = ingest._routing
    result = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]

    ingest.handle_up(up_topic("pgr-v2-5b"), ack_payload(msg.id, "shown"))
    refreshed = messages_store.get_message(msg.id)
    pager_bid = next(bid for bid, d in refreshed.deliveries.items() if d.kind == "pager")
    assert refreshed.deliveries[pager_bid].state == "sent"  # unchanged


def test_v2_ack_idempotent_repeat_is_noop():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-v2-6", "student")

    ingest, _broker = _ingest()
    routing = ingest._routing
    msg = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    ).messages[0]

    ingest.handle_up(up_topic("pgr-v2-6"), ack_payload(msg.id, "shown", ts=1_700_001_000))
    ingest.handle_up(up_topic("pgr-v2-6"), ack_payload(msg.id, "shown", ts=1_700_009_999))
    refreshed = messages_store.get_message(msg.id)
    pager_bid = next(bid for bid, d in refreshed.deliveries.items() if d.kind == "pager")
    assert refreshed.deliveries[pager_bid].shownTs == 1_700_001_000


# ---- v2 online-edge republish ----


def test_v2_republish_on_session_change_resends_still_pending():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-v2-7", "student")

    broker = FakeBrokerClient()
    broker.fail_publish = True
    routing = Routing(broker)
    ingest = Ingest(broker, routing)

    result = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]
    assert broker.published == []  # publish failed -> stayed queued

    broker.fail_publish = False
    ingest.handle_status(status_topic("pgr-v2-7"), online_status_payload("s_00000001"))
    assert len(broker.published) == 1
    assert json.loads(broker.published[0].payload)["id"] == msg.id

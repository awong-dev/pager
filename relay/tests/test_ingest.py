"""`app.ingest.Ingest` against the uid-addressed model (registered
`devices/{id}` docs) -- the drop+system-reply path (PROTOCOL.md §4.2 case 3),
ack routing (find the right pager delivery, monotonic state), and the
online-edge republish (`pendingDeviceIds`). This is the only device-traffic
model; webhook-dispatch plumbing
(auth, parsing, topic routing) is covered by tests/test_webhooks.py, and
`/loc`'s real handling by tests/test_location.py.
"""

from __future__ import annotations

import json
import time

from app import devauth
from app.ingest import Ingest
from app.routing import Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.conftest import (
    ack_payload,
    offline_status_payload,
    online_status_payload,
    status_topic,
    up_message_payload,
    up_topic,
)
from tests.fake_transport import FakeBrokerClient

# Fixed test key -- S1.3's hmac-device tests only need *a* 32-byte key, not
# a provisioned one (that's S2.x's bootstrap flow).
_HMAC_KEY = b"k" * 32


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


def _make_pager_device(device_id: str, owner_uid: str, *, default_to_uid: str | None = None):
    # S1.3: `create_device` now defaults new devices to `authMode: "hmac"`
    # (docs/DEVICE_PLAN.md §2.6); this file's pre-existing tests all send
    # unsigned JSON, so they opt into the v1 "password" mode explicitly.
    # `_make_hmac_pager_device` below is the hmac-signed counterpart.
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        default_to_uid=default_to_uid,
        auth_mode="password",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def _make_hmac_pager_device(
    device_id: str, owner_uid: str, *, key: bytes = _HMAC_KEY, default_to_uid: str | None = None
):
    """An `authMode: "hmac"` device with a `deviceSecrets/{device_id}` row,
    for the S1.3 signed-envelope tests."""
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        default_to_uid=default_to_uid,
        auth_mode="hmac",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )
    device_secrets_store.create(device_id, hmac_key=key, mqtt_password_hash="y")
    return key


def _ingest() -> tuple[Ingest, FakeBrokerClient]:
    broker = FakeBrokerClient()
    routing = Routing(broker)
    return Ingest(broker, routing), broker


def _pager_delivery_state(msg_id: str) -> str:
    msg = messages_store.get_message(msg_id)
    bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "pager")
    return msg.deliveries[bid].state


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


def test_republish_reuses_id_but_gets_a_fresh_n_and_both_publishes_verify():
    """docs/DEVICE_TASKS.md S1.4: every `/down` publish goes through
    `BrokerClient.publish_down`, which signs with a fresh `deviceSecrets.
    downN` each time (docs/DEVICE_PLAN.md §2.5/§14.5) -- the online-edge
    republish (`Routing.redeliver_pager`) reuses the same message `id` but
    must not reuse `n`, and both the original and the republished envelope
    must still verify against the device's key."""
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    key = _make_hmac_pager_device("pgr-hmac-republish", "student")

    broker = FakeBrokerClient()
    routing = Routing(broker)

    msg = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    ).messages[0]
    assert len(broker.published) == 1

    ok = routing.redeliver_pager(msg, "pgr-hmac-republish")
    assert ok
    assert len(broker.published) == 2

    topic = "pager/pgr-hmac-republish/down"
    decoded = []
    for published in broker.published:
        verified, unsigned = devauth.verify(key, topic, published.payload)
        assert verified
        decoded.append(json.loads(unsigned))

    assert decoded[0]["id"] == decoded[1]["id"] == msg.id
    assert decoded[0]["n"] != decoded[1]["n"]


# ---- S1.3: ingest verifies before it parses (DEVICE_PLAN.md §2.6, PROTOCOL.md §14) ----


def test_hmac_valid_signed_json_up_message_is_accepted():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    key = _make_hmac_pager_device("pgr-hmac-1", "student")

    ingest, _broker = _ingest()
    msg = ingest._routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="hi",
        origin_backend_kind="webapp",
    ).messages[0]

    topic = up_topic("pgr-hmac-1")
    payload = devauth.sign_json(
        key, topic, {"v": 1, "id": msg.id, "ts": 1_700_000_100, "ack": "shown", "n": 1}
    )
    ingest.handle_up(topic, payload)

    assert _pager_delivery_state(msg.id) == "shown"
    assert devices_store.get_device("pgr-hmac-1").wire == "json"


def test_hmac_valid_signed_cbor_up_message_is_accepted():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    key = _make_hmac_pager_device("pgr-hmac-2", "student")

    ingest, _broker = _ingest()
    msg = ingest._routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="hi",
        origin_backend_kind="webapp",
    ).messages[0]

    topic = up_topic("pgr-hmac-2")
    payload = devauth.sign_cbor(
        key, topic, {"v": 1, "id": msg.id, "ts": 1_700_000_100, "ack": "shown", "n": 1}
    )
    ingest.handle_up(topic, payload)

    assert _pager_delivery_state(msg.id) == "shown"
    assert devices_store.get_device("pgr-hmac-2").wire == "cbor"


def test_hmac_device_wire_flips_to_cbor_after_one_cbor_envelope():
    key = _make_hmac_pager_device("pgr-hmac-wire", "student2")
    _make_user("student2", "student2")

    ingest, _broker = _ingest()
    topic = up_topic("pgr-hmac-wire")

    json_payload = devauth.sign_json(
        key, topic, {"v": 1, "id": "m_nonexist1", "ts": 1_700_000_001, "ack": "shown", "n": 1}
    )
    ingest.handle_up(topic, json_payload)
    assert devices_store.get_device("pgr-hmac-wire").wire == "json"

    cbor_payload = devauth.sign_cbor(
        key, topic, {"v": 1, "id": "m_nonexist2", "ts": 1_700_000_002, "ack": "shown", "n": 2}
    )
    ingest.handle_up(topic, cbor_payload)
    assert devices_store.get_device("pgr-hmac-wire").wire == "cbor"


def test_unsigned_up_message_from_hmac_device_is_dropped():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_hmac_pager_device("pgr-hmac-3", "student")

    ingest, _broker = _ingest()
    msg = ingest._routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="hi",
        origin_backend_kind="webapp",
    ).messages[0]

    # No `sig` (and no `n`) at all -- the same plain-JSON shape a
    # "password"-mode device would send.
    ingest.handle_up(up_topic("pgr-hmac-3"), ack_payload(msg.id, "shown"))

    assert _pager_delivery_state(msg.id) == "sent"  # unchanged -- dropped before parse
    assert device_secrets_store.get("pgr-hmac-3").sigFailures == 1


def test_replayed_n_is_dropped():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    key = _make_hmac_pager_device("pgr-hmac-4", "student")

    ingest, _broker = _ingest()
    routing = ingest._routing
    msg1 = routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="one",
        origin_backend_kind="webapp",
    ).messages[0]
    msg2 = routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="two",
        origin_backend_kind="webapp",
    ).messages[0]

    topic = up_topic("pgr-hmac-4")
    ingest.handle_up(
        topic,
        devauth.sign_json(
            key, topic, {"v": 1, "id": msg1.id, "ts": 1_700_000_001, "ack": "shown", "n": 5}
        ),
    )
    # Same n=5 again, addressed at a different message -- must not be
    # accepted just because the rest of the envelope differs.
    ingest.handle_up(
        topic,
        devauth.sign_json(
            key, topic, {"v": 1, "id": msg2.id, "ts": 1_700_000_002, "ack": "shown", "n": 5}
        ),
    )

    assert _pager_delivery_state(msg1.id) == "shown"
    assert _pager_delivery_state(msg2.id) == "sent"  # replay dropped


def test_n_inside_window_but_unseen_is_accepted():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    key = _make_hmac_pager_device("pgr-hmac-5", "student")

    ingest, _broker = _ingest()
    routing = ingest._routing
    msg1 = routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="one",
        origin_backend_kind="webapp",
    ).messages[0]
    msg2 = routing.send(
        sender_uid="mom", recipient_alias="student", kind="text", body="two",
        origin_backend_kind="webapp",
    ).messages[0]

    topic = up_topic("pgr-hmac-5")
    ingest.handle_up(
        topic,
        devauth.sign_json(
            key, topic, {"v": 1, "id": msg1.id, "ts": 1_700_000_001, "ack": "shown", "n": 10}
        ),
    )
    # n=8 < upN=10, but the window bit for gap=2 has never been set.
    ingest.handle_up(
        topic,
        devauth.sign_json(
            key, topic, {"v": 1, "id": msg2.id, "ts": 1_700_000_002, "ack": "shown", "n": 8}
        ),
    )

    assert _pager_delivery_state(msg1.id) == "shown"
    assert _pager_delivery_state(msg2.id) == "shown"


def test_unsigned_lwt_status_is_accepted_from_hmac_device():
    key = _make_hmac_pager_device("pgr-hmac-lwt", "student3")
    _make_user("student3", "student3")

    ingest, _broker = _ingest()
    status_t = status_topic("pgr-hmac-lwt")
    # Bring the device online first (signed), so the offline LWT is a real
    # transition worth observing.
    online = devauth.sign_json(
        key,
        status_t,
        {
            "v": 1,
            "state": "online",
            "mode": "sleep",
            "batt_mv": 3300,
            "rssi": -90,
            "session": "s_00000001",
            "ts": 1_700_000_000,
            "n": 1,
        },
    )
    ingest.handle_status(status_t, online)
    assert devices_store.get_device("pgr-hmac-lwt").status.state == "online"

    # The broker-generated LWT: unsigned, exactly {v, state, session}.
    ingest.handle_status(status_t, offline_status_payload("s_00000001"))

    device = devices_store.get_device("pgr-hmac-lwt")
    assert device.status.state == "offline"
    assert device.status.authAlarm is not True
    assert device_secrets_store.get("pgr-hmac-lwt").sigFailures == 0

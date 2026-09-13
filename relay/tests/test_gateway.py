"""Tests for app.mqtt_gateway against tests.fake_transport.FakeTransport.

Covers: send -> publish -> sent-on-PUBACK, the four down-message states
reached in order, out-of-order read-without-shown, idempotent repeat acks,
unknown-id acks, wrong-device acks, malformed payloads (never crashing the
loop), duplicate up-message ids, and the online-edge/session-change
republish rule (cap + ordering).
"""

from __future__ import annotations

import json
import time

from app.mqtt_gateway import MqttGateway
from app.store import Store
from tests.conftest import (
    ack_payload,
    down_topic,
    offline_status_payload,
    online_status_payload,
    status_topic,
    up_message_payload,
    up_topic,
)
from tests.fake_transport import FakeTransport


def test_gateway_subscribes_on_connect(gateway: MqttGateway, transport: FakeTransport):
    assert ("pager/+/up", 1) in transport.subscriptions
    assert ("pager/+/status", 1) in transport.subscriptions


def test_send_then_puback_transitions_to_sent(store: Store, gateway: MqttGateway, transport: FakeTransport):
    row = store.create_down_message(msg_id="m_aaaaaaaa", device_id="pgr-0001", ts=int(time.time()), body="hi")
    assert row.state == "queued"

    gateway.publish_down(row)
    assert store.get_message("m_aaaaaaaa").state == "queued"  # not sent until PUBACK

    transport.ack_last()
    assert store.get_message("m_aaaaaaaa").state == "sent"


def test_full_state_progression_queued_sent_shown_read(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    row = store.create_down_message(msg_id="m_bbbbbbbb", device_id=device_id, ts=int(time.time()), body="hi")
    assert store.get_message(row.id).state == "queued"

    gateway.publish_down(row)
    transport.ack_last()
    assert store.get_message(row.id).state == "sent"

    transport.deliver(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_000_100))
    shown_row = store.get_message(row.id)
    assert shown_row.state == "shown"
    assert shown_row.shown_ts == 1_700_000_100

    transport.deliver(up_topic(device_id), ack_payload(row.id, "read", ts=1_700_000_200))
    read_row = store.get_message(row.id)
    assert read_row.state == "read"
    assert read_row.read_ts == 1_700_000_200
    assert read_row.shown_ts == 1_700_000_100  # untouched, not backfilled


def test_out_of_order_read_without_shown_backfills_shown_ts(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    row = store.create_down_message(msg_id="m_cccccccc", device_id=device_id, ts=int(time.time()), body="hi")
    gateway.publish_down(row)
    transport.ack_last()

    transport.deliver(up_topic(device_id), ack_payload(row.id, "read", ts=1_700_000_300))
    updated = store.get_message(row.id)
    assert updated.state == "read"
    assert updated.read_ts == 1_700_000_300
    assert updated.shown_ts == 1_700_000_300  # backfilled

    # A late 'shown' arriving after 'read' must be ignored (state stays read,
    # shown_ts stays backfilled).
    transport.deliver(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_000_999))
    after = store.get_message(row.id)
    assert after.state == "read"
    assert after.shown_ts == 1_700_000_300


def test_idempotent_repeat_ack_is_noop(store: Store, gateway: MqttGateway, transport: FakeTransport):
    device_id = "pgr-0001"
    row = store.create_down_message(msg_id="m_dddddddd", device_id=device_id, ts=int(time.time()), body="hi")
    gateway.publish_down(row)
    transport.ack_last()

    transport.deliver(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_001_000))
    first = store.get_message(row.id)
    assert first.state == "shown"
    assert first.shown_ts == 1_700_001_000

    # Repeat the same ack -- must be a no-op (idempotent), not an error, and
    # must not change shown_ts.
    transport.deliver(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_009_999))
    second = store.get_message(row.id)
    assert second.state == "shown"
    assert second.shown_ts == 1_700_001_000


def test_unknown_id_ack_dropped_without_creating_row(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    transport.deliver(up_topic(device_id), ack_payload("m_ffffffff", "shown"))
    assert store.get_message("m_ffffffff") is None


def test_wrong_device_ack_dropped(store: Store, gateway: MqttGateway, transport: FakeTransport):
    owner_device = "pgr-0001"
    attacker_device = "pgr-0002"
    row = store.create_down_message(msg_id="m_eeeeeeee", device_id=owner_device, ts=int(time.time()), body="hi")
    gateway.publish_down(row)
    transport.ack_last()
    assert store.get_message(row.id).state == "sent"

    transport.deliver(up_topic(attacker_device), ack_payload(row.id, "shown"))

    unchanged = store.get_message(row.id)
    assert unchanged.state == "sent"  # not promoted to shown


def test_malformed_payloads_logged_and_dropped_without_crashing(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"

    # not valid JSON
    transport.deliver(up_topic(device_id), b"not json{{{")
    # too large (> 640 bytes hard cap)
    transport.deliver(up_topic(device_id), b'{"v":1,"id":"m_aaaaaaaa","ts":1700000000,"from":"student","body":"' + b"x" * 700 + b'","ack":null}')
    # missing required fields (no ts, no ack)
    transport.deliver(up_topic(device_id), b'{"id":"m_aaaaaaaa"}')
    # both ack and body set -> malformed per §3.2
    transport.deliver(
        up_topic(device_id),
        b'{"v":1,"id":"m_aaaaaaaa","ts":1700000000,"ack":"shown","body":"nope"}',
    )
    # not a JSON object (array)
    transport.deliver(up_topic(device_id), b"[1,2,3]")
    # invalid UTF-8
    transport.deliver(up_topic(device_id), b"\xff\xfe\x00\x01")

    # None of the above should have created any row.
    assert store.get_thread(device_id) == []

    # The loop must still work after all that garbage.
    row = store.create_down_message(msg_id="m_aaaaaaaa", device_id=device_id, ts=int(time.time()), body="hi")
    gateway.publish_down(row)
    transport.ack_last()
    transport.deliver(up_topic(device_id), ack_payload(row.id, "shown"))
    assert store.get_message(row.id).state == "shown"


def test_duplicate_up_message_id_dropped_silently(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    payload = up_message_payload("u_11111111", "ok coming")
    transport.deliver(up_topic(device_id), payload)
    transport.deliver(up_topic(device_id), payload)  # QoS 1 redelivery

    rows = store.get_thread(device_id)
    assert len(rows) == 1
    assert rows[0].id == "u_11111111"


def test_republish_on_new_session_respects_cap_and_oldest_first_order(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    now = int(time.time())

    # 12 unacked messages, oldest first by creation order.
    ids = []
    for i in range(12):
        msg_id = f"m_{i:08x}"
        store.create_down_message(msg_id=msg_id, device_id=device_id, ts=now, body=f"msg {i}", now=now + i)
        ids.append(msg_id)

    transport.published.clear()
    transport.deliver(status_topic(device_id), online_status_payload("s_00000001"))

    assert len(transport.published) == 10  # capped
    republished_ids = [p.topic for p in transport.published]
    assert all(topic == down_topic(device_id) for topic in republished_ids)

    # oldest-first: first 10 of the 12 created ids, in order.
    payload_ids = [json.loads(p.payload)["id"] for p in transport.published]
    assert payload_ids == ids[:10]


def test_republish_triggered_by_session_change(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    row = store.create_down_message(msg_id="m_session1", device_id=device_id, ts=int(time.time()), body="hi")
    gateway.publish_down(row)
    transport.ack_last()

    transport.deliver(status_topic(device_id), online_status_payload("s_00000001"))
    transport.published.clear()

    # Same session again -> no republish.
    transport.deliver(status_topic(device_id), online_status_payload("s_00000001"))
    assert transport.published == []

    # New session -> republish of the still-unacked message.
    transport.deliver(status_topic(device_id), online_status_payload("s_00000002"))
    assert len(transport.published) == 1


def test_republish_triggered_by_offline_to_online_edge_same_session(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    row = store.create_down_message(msg_id="m_offline1", device_id=device_id, ts=int(time.time()), body="hi")
    gateway.publish_down(row)
    transport.ack_last()

    transport.deliver(status_topic(device_id), online_status_payload("s_00000001"))
    transport.published.clear()

    transport.deliver(status_topic(device_id), offline_status_payload("s_00000001"))
    assert transport.published == []  # bare offline never republishes

    # Same session, but an offline->online edge -> republish.
    transport.deliver(status_topic(device_id), online_status_payload("s_00000001"))
    assert len(transport.published) == 1


def test_republish_excludes_already_acked_and_expired_messages(
    store: Store, gateway: MqttGateway, transport: FakeTransport
):
    device_id = "pgr-0001"
    now = int(time.time())

    acked = store.create_down_message(msg_id="m_acked000", device_id=device_id, ts=now, body="acked", now=now)
    store.apply_ack(acked.id, "shown", now)

    expired = store.create_down_message(
        msg_id="m_expired0", device_id=device_id, ts=now, body="old", now=now - 90_000
    )

    fresh = store.create_down_message(msg_id="m_fresh000", device_id=device_id, ts=now, body="fresh", now=now)

    transport.deliver(status_topic(device_id), online_status_payload("s_00000001"))

    payload_ids = {json.loads(p.payload)["id"] for p in transport.published}
    assert payload_ids == {fresh.id}
    assert acked.id not in payload_ids
    assert expired.id not in payload_ids

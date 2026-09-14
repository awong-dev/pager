"""Tests for app.ingest.Ingest against tests.fake_transport.FakeBrokerClient
and the Firestore emulator (via app.store.legacy).

Covers: send -> publish (sent on a successful broker REST call, queued on a
failed one), the four down-message states reached in order, out-of-order
read-without-shown, idempotent repeat acks, unknown-id acks, wrong-device
acks, malformed payloads (never crashing), duplicate up-message ids, the
online-edge/session-change republish rule (cap + ordering), and the `/loc`
stub.
"""

from __future__ import annotations

import json
import time

from app.ingest import Ingest
from app.store import legacy as legacy_store
from tests.conftest import (
    ack_payload,
    down_topic,
    loc_payload,
    loc_topic,
    offline_status_payload,
    online_status_payload,
    status_topic,
    up_message_payload,
    up_topic,
)
from tests.fake_transport import FakeBrokerClient


def test_publish_down_success_transitions_to_sent(ingest: Ingest, broker: FakeBrokerClient):
    row = legacy_store.create_down_message(
        msg_id="m_aaaaaaaa", device_id="pgr-0001", ts=int(time.time()), body="hi"
    )
    assert row.state == "queued"

    ok = ingest.publish_down(row)
    assert ok is True
    assert legacy_store.get_message("m_aaaaaaaa").state == "sent"
    assert len(broker.published) == 1
    assert broker.published[0].topic == "pager/pgr-0001/down"


def test_publish_down_failure_leaves_queued(ingest: Ingest, broker: FakeBrokerClient):
    row = legacy_store.create_down_message(
        msg_id="m_offline0", device_id="pgr-0001", ts=int(time.time()), body="hi"
    )
    broker.fail_publish = True

    ok = ingest.publish_down(row)
    assert ok is False
    assert legacy_store.get_message("m_offline0").state == "queued"


def test_full_state_progression_queued_sent_shown_read(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    row = legacy_store.create_down_message(
        msg_id="m_bbbbbbbb", device_id=device_id, ts=int(time.time()), body="hi"
    )
    assert legacy_store.get_message(row.id).state == "queued"

    ingest.publish_down(row)
    assert legacy_store.get_message(row.id).state == "sent"

    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_000_100))
    shown_row = legacy_store.get_message(row.id)
    assert shown_row.state == "shown"
    assert shown_row.shownTs == 1_700_000_100

    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "read", ts=1_700_000_200))
    read_row = legacy_store.get_message(row.id)
    assert read_row.state == "read"
    assert read_row.readTs == 1_700_000_200
    assert read_row.shownTs == 1_700_000_100  # untouched, not backfilled


def test_out_of_order_read_without_shown_backfills_shown_ts(
    ingest: Ingest, broker: FakeBrokerClient
):
    device_id = "pgr-0001"
    row = legacy_store.create_down_message(
        msg_id="m_cccccccc", device_id=device_id, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)

    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "read", ts=1_700_000_300))
    updated = legacy_store.get_message(row.id)
    assert updated.state == "read"
    assert updated.readTs == 1_700_000_300
    assert updated.shownTs == 1_700_000_300  # backfilled

    # A late 'shown' arriving after 'read' must be ignored (state stays read,
    # shownTs stays backfilled).
    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_000_999))
    after = legacy_store.get_message(row.id)
    assert after.state == "read"
    assert after.shownTs == 1_700_000_300


def test_idempotent_repeat_ack_is_noop(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    row = legacy_store.create_down_message(
        msg_id="m_dddddddd", device_id=device_id, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)

    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_001_000))
    first = legacy_store.get_message(row.id)
    assert first.state == "shown"
    assert first.shownTs == 1_700_001_000

    # Repeat the same ack -- must be a no-op (idempotent), not an error, and
    # must not change shownTs.
    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "shown", ts=1_700_009_999))
    second = legacy_store.get_message(row.id)
    assert second.state == "shown"
    assert second.shownTs == 1_700_001_000


def test_unknown_id_ack_dropped_without_creating_row(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    ingest.handle_up(up_topic(device_id), ack_payload("m_ffffffff", "shown"))
    assert legacy_store.get_message("m_ffffffff") is None


def test_wrong_device_ack_dropped(ingest: Ingest, broker: FakeBrokerClient):
    owner_device = "pgr-0001"
    attacker_device = "pgr-0002"
    row = legacy_store.create_down_message(
        msg_id="m_eeeeeeee", device_id=owner_device, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)
    assert legacy_store.get_message(row.id).state == "sent"

    ingest.handle_up(up_topic(attacker_device), ack_payload(row.id, "shown"))

    unchanged = legacy_store.get_message(row.id)
    assert unchanged.state == "sent"  # not promoted to shown


def test_malformed_payloads_logged_and_dropped_without_crashing(
    ingest: Ingest, broker: FakeBrokerClient
):
    device_id = "pgr-0001"

    # not valid JSON
    ingest.handle_up(up_topic(device_id), b"not json{{{")
    # too large (> 640 bytes hard cap)
    ingest.handle_up(
        up_topic(device_id),
        b'{"v":1,"id":"m_aaaaaaaa","ts":1700000000,"from":"student","body":"'
        + b"x" * 700
        + b'","ack":null}',
    )
    # missing required fields (no ts, no ack)
    ingest.handle_up(up_topic(device_id), b'{"id":"m_aaaaaaaa"}')
    # both ack and body set -> malformed per §3.2
    ingest.handle_up(
        up_topic(device_id),
        b'{"v":1,"id":"m_aaaaaaaa","ts":1700000000,"ack":"shown","body":"nope"}',
    )
    # not a JSON object (array)
    ingest.handle_up(up_topic(device_id), b"[1,2,3]")
    # invalid UTF-8
    ingest.handle_up(up_topic(device_id), b"\xff\xfe\x00\x01")
    # unrecognised topic shape
    ingest.handle_up("pager/bad", b'{"v":1}')

    # None of the above should have created any row.
    assert legacy_store.get_thread(device_id) == []

    # Ingest must still work after all that garbage.
    row = legacy_store.create_down_message(
        msg_id="m_aaaaaaaa", device_id=device_id, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)
    ingest.handle_up(up_topic(device_id), ack_payload(row.id, "shown"))
    assert legacy_store.get_message(row.id).state == "shown"


def test_duplicate_up_message_id_dropped_silently(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    payload = up_message_payload("u_11111111", "ok coming")
    ingest.handle_up(up_topic(device_id), payload)
    ingest.handle_up(up_topic(device_id), payload)  # QoS 1 redelivery

    rows = legacy_store.get_thread(device_id)
    assert len(rows) == 1
    assert rows[0].id == "u_11111111"


def test_republish_on_new_session_respects_cap_and_oldest_first_order(
    ingest: Ingest, broker: FakeBrokerClient
):
    device_id = "pgr-0001"
    now = int(time.time())

    # 12 unacked messages, oldest first by creation order.
    ids = []
    for i in range(12):
        msg_id = f"m_{i:08x}"
        legacy_store.create_down_message(
            msg_id=msg_id, device_id=device_id, ts=now, body=f"msg {i}", now=now + i
        )
        ids.append(msg_id)

    broker.clear()
    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000001"))

    assert len(broker.published) == 10  # capped
    assert all(p.topic == down_topic(device_id) for p in broker.published)

    # oldest-first: first 10 of the 12 created ids, in order.
    payload_ids = [json.loads(p.payload)["id"] for p in broker.published]
    assert payload_ids == ids[:10]


def test_republish_triggered_by_session_change(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    row = legacy_store.create_down_message(
        msg_id="m_session1", device_id=device_id, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)

    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000001"))
    broker.clear()

    # Same session again -> no republish.
    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000001"))
    assert broker.published == []

    # New session -> republish of the still-unacked message.
    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000002"))
    assert len(broker.published) == 1


def test_republish_triggered_by_offline_to_online_edge_same_session(
    ingest: Ingest, broker: FakeBrokerClient
):
    device_id = "pgr-0001"
    row = legacy_store.create_down_message(
        msg_id="m_offline1", device_id=device_id, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)

    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000001"))
    broker.clear()

    ingest.handle_status(status_topic(device_id), offline_status_payload("s_00000001"))
    assert broker.published == []  # bare offline never republishes

    # Same session, but an offline->online edge -> republish.
    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000001"))
    assert len(broker.published) == 1


def test_republish_excludes_already_acked_and_expired_messages(
    ingest: Ingest, broker: FakeBrokerClient
):
    device_id = "pgr-0001"
    now = int(time.time())

    acked = legacy_store.create_down_message(
        msg_id="m_acked000", device_id=device_id, ts=now, body="acked", now=now
    )
    legacy_store.apply_ack(acked.id, "shown", now)

    expired = legacy_store.create_down_message(
        msg_id="m_expired0", device_id=device_id, ts=now, body="old", now=now - 90_000
    )

    fresh = legacy_store.create_down_message(
        msg_id="m_fresh000", device_id=device_id, ts=now, body="fresh", now=now
    )

    ingest.handle_status(status_topic(device_id), online_status_payload("s_00000001"))

    payload_ids = {json.loads(p.payload)["id"] for p in broker.published}
    assert payload_ids == {fresh.id}
    assert acked.id not in payload_ids
    assert expired.id not in payload_ids


# ---- retry_queued (the lazy-at-read-time /internal/tick stand-in) ----


def test_retry_queued_publishes_still_queued_messages_only(
    ingest: Ingest, broker: FakeBrokerClient
):
    device_id = "pgr-0001"
    broker.fail_publish = True
    row = legacy_store.create_down_message(
        msg_id="m_retryq00", device_id=device_id, ts=int(time.time()), body="hi"
    )
    ingest.publish_down(row)
    assert legacy_store.get_message(row.id).state == "queued"

    already_sent = legacy_store.create_down_message(
        msg_id="m_alreadys", device_id=device_id, ts=int(time.time()), body="already sent"
    )
    broker.fail_publish = False
    ingest.publish_down(already_sent)
    broker.clear()

    ingest.retry_queued(device_id)

    # Only the still-'queued' message is retried -- the already-'sent'
    # message must not be re-published just because someone did a GET.
    assert legacy_store.get_message(row.id).state == "sent"
    assert len(broker.published) == 1
    assert broker.published[0].topic == down_topic(device_id)
    assert json.loads(broker.published[0].payload)["id"] == row.id


def test_retry_queued_is_a_noop_when_nothing_is_queued(ingest: Ingest, broker: FakeBrokerClient):
    # Must not raise for a device with no messages at all.
    ingest.retry_queued("pgr-9999")
    assert broker.published == []


# ---- /loc stub ----


def test_loc_valid_payload_is_a_noop(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    # Must not raise, must not touch the store or publish anything -- real
    # handling is docs/SERVER_PLAN.md §5.6, Phase 4.
    ingest.handle_loc(loc_topic(device_id), loc_payload("l_11111111"))
    assert broker.published == []
    assert legacy_store.get_thread(device_id) == []


def test_loc_malformed_payload_dropped_without_crashing(ingest: Ingest, broker: FakeBrokerClient):
    device_id = "pgr-0001"
    ingest.handle_loc(loc_topic(device_id), b"not json{{{")
    ingest.handle_loc(loc_topic(device_id), b'{"v":1,"id":"l_11111111"}')  # missing loc/req
    assert broker.published == []

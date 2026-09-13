"""Direct unit tests of the Store, independent of MQTT: thread ordering
(insertion order, not ts, per PROTOCOL.md §3.5) and lazy expiry."""

from __future__ import annotations

from app.store import EXPIRY_SECONDS, Store


def test_thread_order_is_insertion_order_not_ts(store: Store):
    device_id = "pgr-0001"
    # Insert with ts values that are NOT in ascending insertion order.
    store.create_down_message(msg_id="m_11111111", device_id=device_id, ts=5000, body="third-by-ts")
    store.create_down_message(msg_id="m_22222222", device_id=device_id, ts=1000, body="first-by-ts")
    store.create_down_message(msg_id="m_33333333", device_id=device_id, ts=3000, body="second-by-ts")

    thread = store.get_thread(device_id)
    assert [m.id for m in thread] == ["m_11111111", "m_22222222", "m_33333333"]


def test_since_filters_by_created_at(store: Store):
    device_id = "pgr-0001"
    store.create_down_message(msg_id="m_aaaaaaaa", device_id=device_id, ts=1, body="old", now=1000)
    store.create_down_message(msg_id="m_bbbbbbbb", device_id=device_id, ts=2, body="new", now=2000)

    thread = store.get_thread(device_id, since=1000)
    assert [m.id for m in thread] == ["m_bbbbbbbb"]


def test_expired_is_computed_lazily_not_persisted(store: Store):
    device_id = "pgr-0001"
    old_ts = 1_000_000
    row = store.create_down_message(
        msg_id="m_aaaaaaaa", device_id=device_id, ts=old_ts, body="stale", now=old_ts
    )
    assert row.state == "queued"  # persisted state never becomes "expired"

    now = old_ts + EXPIRY_SECONDS + 1
    assert row.effective_state(now=now) == "expired"
    assert row.effective_state(now=old_ts + 10) == "queued"


def test_id_exists_and_get_message(store: Store):
    assert not store.id_exists("m_aaaaaaaa")
    store.create_down_message(msg_id="m_aaaaaaaa", device_id="pgr-0001", ts=1, body="hi")
    assert store.id_exists("m_aaaaaaaa")
    assert store.get_message("m_aaaaaaaa") is not None
    assert store.get_message("m_missing00") is None

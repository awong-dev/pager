"""Direct unit tests of app.store.legacy against the Firestore emulator:
thread ordering (insertion order, not ts, per PROTOCOL.md §3.5), lazy
expiry, and the id-dedup guarantee."""

from __future__ import annotations

import threading

from app.store import legacy as legacy_store
from app.store.legacy import EXPIRY_SECONDS


def test_thread_order_is_insertion_order_not_ts():
    device_id = "pgr-0001"
    # Insert with ts values that are NOT in ascending insertion order.
    legacy_store.create_down_message(
        msg_id="m_11111111", device_id=device_id, ts=5000, body="third-by-ts"
    )
    legacy_store.create_down_message(
        msg_id="m_22222222", device_id=device_id, ts=1000, body="first-by-ts"
    )
    legacy_store.create_down_message(
        msg_id="m_33333333", device_id=device_id, ts=3000, body="second-by-ts"
    )

    thread = legacy_store.get_thread(device_id)
    assert [m.id for m in thread] == ["m_11111111", "m_22222222", "m_33333333"]


def test_since_filters_by_created_at():
    device_id = "pgr-0001"
    legacy_store.create_down_message(
        msg_id="m_aaaaaaaa", device_id=device_id, ts=1, body="old", now=1000
    )
    legacy_store.create_down_message(
        msg_id="m_bbbbbbbb", device_id=device_id, ts=2, body="new", now=2000
    )

    thread = legacy_store.get_thread(device_id, since=1000)
    assert [m.id for m in thread] == ["m_bbbbbbbb"]


def test_expired_is_computed_lazily_not_persisted():
    device_id = "pgr-0001"
    old_ts = 1_000_000
    row = legacy_store.create_down_message(
        msg_id="m_aaaaaaaa", device_id=device_id, ts=old_ts, body="stale", now=old_ts
    )
    assert row.state == "queued"  # persisted state never becomes "expired"

    now = old_ts + EXPIRY_SECONDS + 1
    assert row.effective_state(now=now) == "expired"
    assert row.effective_state(now=old_ts + 10) == "queued"


def test_id_exists_and_get_message():
    assert not legacy_store.id_exists("m_aaaaaaaa")
    legacy_store.create_down_message(msg_id="m_aaaaaaaa", device_id="pgr-0001", ts=1, body="hi")
    assert legacy_store.id_exists("m_aaaaaaaa")
    assert legacy_store.get_message("m_aaaaaaaa") is not None
    assert legacy_store.get_message("m_missing00") is None


def test_insert_up_message_returns_true_then_false_on_duplicate():
    assert (
        legacy_store.insert_up_message(
            msg_id="u_dup00001", device_id="pgr-0001", ts=1, sender="student", body="hi"
        )
        is True
    )
    assert (
        legacy_store.insert_up_message(
            msg_id="u_dup00001", device_id="pgr-0001", ts=2, sender="student", body="again"
        )
        is False
    )
    rows = legacy_store.get_thread("pgr-0001")
    assert len(rows) == 1
    assert rows[0].body == "hi"  # the second (duplicate) insert never applied


def test_concurrent_insert_up_message_same_id_only_one_succeeds():
    """Two concurrent at-least-once webhook deliveries of the same
    up-message id must not race past a check-then-act dedup check -- the
    insert itself (`transaction.create()`, which raises `AlreadyExists` if
    the document exists by commit time) must be the atomic source of
    truth. Exactly one of the two calls returns True (inserted), the other
    False (duplicate), and neither raises."""
    results: list[bool] = []
    errors: list[BaseException] = []
    barrier = threading.Barrier(2)

    def worker() -> None:
        barrier.wait()
        try:
            results.append(
                legacy_store.insert_up_message(
                    msg_id="u_racecase", device_id="pgr-0001", ts=1, sender="student", body="hi"
                )
            )
        except BaseException as exc:  # noqa: BLE001 -- captured for the assertion below
            errors.append(exc)

    threads = [threading.Thread(target=worker) for _ in range(2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert errors == []
    assert sorted(results) == [False, True]
    rows = legacy_store.get_thread("pgr-0001")
    assert len(rows) == 1
    assert rows[0].id == "u_racecase"

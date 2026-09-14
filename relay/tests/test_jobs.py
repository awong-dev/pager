"""`app.jobs.tick()`: retries pager deliveries still 'queued', at most 10 per
device, oldest first -- docs/SERVER_PLAN.md §5.8 item 1 -- plus this phase's
(5) addition retrying non-pager (`sms`) deliveries still 'queued' (see
`app/jobs.py`'s module docstring). Uses `app.tasks.InlineTaskQueue` (the
only mode this phase implements) so retries run synchronously and are
observable immediately.

`app.jobs.sweep()` (docs/SERVER_PLAN.md §5.7) is covered further down:
batching with a small `SWEEP_BATCH`, idempotency across two consecutive
calls, day-vs-week unit conversion, and `locWireIds`/`locReqs` being swept
alongside `locations` (the phase-4-review finding `app/jobs.py`'s docstring
cites). Test data is inserted directly through the Firestore admin SDK
(`get_db()`) with an explicit past `createdAt` -- there is no store-layer
API for "create a message/location from N days ago" (every real write path
stamps `SERVER_TIMESTAMP`), so backdating a document's `createdAt` for a
retention test has to go around those APIs, the same whitebox approach
`tools/e2e_v2.py`'s `Oracle`/`_backdate_*` helpers use at the integration
level.
"""

from __future__ import annotations

import json
import time as _time
from datetime import UTC, datetime, timedelta

from app import jobs
from app.backends.sms_twilio import SmsTwilioBackend
from app.db.firestore import get_db
from app.routing import Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import locations as locations_store
from app.store import messages as messages_store
from app.store import settings as settings_store
from app.store import users as users_store
from app.store.settings import RetentionSetting
from app.tasks import InlineTaskQueue
from tests.fake_transport import FakeBrokerClient


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


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


def test_tick_retries_only_queued_deliveries_capped_oldest_first():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-tick-1", "student")

    broker = FakeBrokerClient()
    broker.fail_publish = True
    routing = Routing(broker)

    ids = []
    for i in range(12):
        result = routing.send(
            sender_uid="mom",
            recipient_alias="student",
            kind="text",
            body=f"msg {i}",
            origin_backend_kind="webapp",
            wire_id=f"w_tick_{i}",
        )
        ids.append(result.messages[0].id)
    assert broker.published == []  # every publish attempt failed -> all queued

    broker.fail_publish = False
    result = jobs.tick(routing, task_queue=InlineTaskQueue())

    assert result.devicesChecked == 1
    assert result.retriesAttempted == 10  # capped
    assert len(broker.published) == 10
    payload_ids = [json.loads(p.payload)["id"] for p in broker.published]
    assert payload_ids == ids[:10]  # oldest first


def test_tick_does_not_retry_already_sent_deliveries():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-tick-2", "student")

    broker = FakeBrokerClient()
    routing = Routing(broker)
    routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert len(broker.published) == 1  # delivered successfully -> 'sent'
    broker.clear()

    result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert result.retriesAttempted == 0
    assert broker.published == []


def test_tick_gives_up_after_five_failed_attempts():
    """S2b: a pager delivery that can never succeed (broker permanently
    unreachable, standing in for e.g. broken credentials) must not be
    retried forever -- after the 5th failed attempt it is marked 'failed'
    and dropped from pendingDeviceIds, so a 6th tick() is a no-op for it."""
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-tick-fail", "student")

    broker = FakeBrokerClient()
    broker.fail_publish = True
    routing = Routing(broker)
    result = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg_id = result.messages[0].id
    pager_bid = next(
        bid for bid, d in result.messages[0].deliveries.items() if d.kind == "pager"
    )
    # The inline delivery attempt at send() time already counts as attempt 1.
    msg = messages_store.get_message(msg_id)
    assert msg.deliveries[pager_bid].attempts == 1
    assert msg.deliveries[pager_bid].error == "broker publish failed"

    # 4 more tick()s -> attempt 5 -> 'failed', device dropped from
    # pendingDeviceIds.
    for _ in range(4):
        tick_result = jobs.tick(routing, task_queue=InlineTaskQueue())
        assert tick_result.retriesAttempted == 1

    msg = messages_store.get_message(msg_id)
    assert msg.deliveries[pager_bid].attempts == 5
    assert msg.deliveries[pager_bid].state == "failed"
    assert "pgr-tick-fail" not in msg.pendingDeviceIds

    # A 6th tick is a no-op: the message no longer matches
    # `pendingDeviceIds array-contains pgr-tick-fail` at all.
    final_tick = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert final_tick.retriesAttempted == 0
    assert final_tick.devicesChecked == 1


def test_tick_skips_revoked_devices():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-tick-3", "student")

    broker = FakeBrokerClient()
    broker.fail_publish = True
    routing = Routing(broker)
    routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    devices_store.revoke_device("pgr-tick-3")

    result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert result.devicesChecked == 0
    assert result.retriesAttempted == 0


# ---------------------------------------------------------------------------
# tick(): non-pager (sms) retry -- this phase's (5) addition, see
# app/jobs.py's module docstring
# ---------------------------------------------------------------------------


def test_tick_retries_queued_sms_delivery(monkeypatch):
    """No TWILIO_BASE_URL configured -> SmsTwilioBackend.deliver() leaves the
    delivery 'queued' (not 'failed' -- see that module's docstring) on the
    inline send; tick() must find and retry it via `Routing.redeliver`, the
    same way it retries a queued pager delivery via `redeliver_pager`."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    backends_store.create_backend(
        "student", kind="sms", config={"phone": "+15551234567"}, enabled=True
    )

    routing = Routing(FakeBrokerClient())
    result = routing.send(
        sender_uid="mom",
        recipient_alias="student",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]
    sms_bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "sms")
    assert messages_store.get_message(msg.id).deliveries[sms_bid].state == "queued"

    tick_result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert tick_result.nonPagerRetriesAttempted == 1
    # Still queued (no TWILIO_BASE_URL) -- but attempts advanced, proving
    # tick() actually re-invoked deliver() rather than skipping it.
    refreshed = messages_store.get_message(msg.id)
    assert refreshed.deliveries[sms_bid].state == "queued"
    assert refreshed.deliveries[sms_bid].attempts == 2


def test_tick_caps_non_pager_retries_dispatched_per_call(monkeypatch):
    """M3 (build review, phase 6): a single `tick()` call must not *dispatch*
    more than `jobs.NON_PAGER_RETRY_DISPATCH_LIMIT` (10) non-pager retries,
    even when more than that many are queued and scan-eligible -- mirrors
    §5.8's existing "at most 10 per device" pager cap, for the same reason
    (bound worst-case tick duration: `NON_PAGER_RETRY_SCAN_LIMIT` (50) *
    `app/notify/sms.py`'s `REQUEST_TIMEOUT_S` (5s) could otherwise approach ~250s, close
    to Cloud Run's default 300s request timeout, if the broker and the
    Twilio mock were both down at once)."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    _make_user("mom3", "mom3")
    _make_user("student3", "student3")
    allow_store.set_edge("mom3", "student3", message=True, locate=True)
    backends_store.create_backend(
        "student3", kind="sms", config={"phone": "+15550001111"}, enabled=True
    )

    routing = Routing(FakeBrokerClient())
    for i in range(20):
        routing.send(
            sender_uid="mom3",
            recipient_alias="student3",
            kind="text",
            body=f"msg {i}",
            origin_backend_kind="webapp",
        )

    tick_result = jobs.tick(routing, task_queue=InlineTaskQueue())
    # Exactly the cap, not "at most" -- 20 were queued and scan-eligible
    # (well within NON_PAGER_RETRY_SCAN_LIMIT=50), so if the cap weren't
    # enforced this would be 20.
    assert tick_result.nonPagerRetriesAttempted == 10
    assert tick_result.nonPagerRetriesAttempted == jobs.NON_PAGER_RETRY_DISPATCH_LIMIT


def test_tick_does_not_retry_sent_sms_delivery(monkeypatch):
    monkeypatch.setenv("TWILIO_BASE_URL", "http://sms-mock.invalid")
    _make_user("mom2", "mom2")
    _make_user("student2", "student2")
    allow_store.set_edge("mom2", "student2", message=True, locate=True)
    backends_store.create_backend(
        "student2", kind="sms", config={"phone": "+15550000000"}, enabled=True
    )

    class _FakeSmsBackend(SmsTwilioBackend):
        def deliver(self, msg, delivery, backend):  # type: ignore[override]
            messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
            from app.backends.base import DeliverResult

            return DeliverResult(ok=True, state="sent")

    routing = Routing(FakeBrokerClient(), registry={"sms": _FakeSmsBackend()})
    result = routing.send(
        sender_uid="mom2",
        recipient_alias="student2",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]
    sms_bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "sms")
    assert messages_store.get_message(msg.id).deliveries[sms_bid].state == "sent"

    tick_result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert tick_result.nonPagerRetriesAttempted == 0


# ---------------------------------------------------------------------------
# sweep() -- docs/SERVER_PLAN.md §5.7
# ---------------------------------------------------------------------------


def _insert_stale_message(
    sender_uid: str, recipient_uid: str, *, days_ago: float, wire_id: str | None = None
) -> str:
    from app.ids import new_id

    msg_id = new_id("m_")
    key = messages_store.conv_key(sender_uid, recipient_uid)
    doc = {
        "seq": 1,
        "convKey": key,
        "uids": sorted([sender_uid, recipient_uid]),
        "senderUid": sender_uid,
        "recipientUid": recipient_uid,
        "kind": "text",
        "body": "stale",
        "loc": None,
        "wireId": wire_id,
        "originBackendKind": "webapp",
        "originBackendId": None,
        "ts": int(_time.time()),
        "createdAt": datetime.now(UTC) - timedelta(days=days_ago),
        "deliveries": {},
        "pendingDeviceIds": [],
    }
    get_db().collection("messages").document(msg_id).set(doc)
    if wire_id:
        get_db().collection("wireIds").document(f"{wire_id}_{recipient_uid}").set(
            {"messageId": msg_id}
        )
    return msg_id


def _insert_stale_location(device_id: str, *, days_ago: float) -> str:
    ts = int(_time.time())
    doc = {
        "ts": ts,
        "fixTs": ts,
        "lat": 1.0,
        "lon": 2.0,
        "accM": 5,
        "src": "gnss",
        "cached": False,
        "reqId": None,
        "createdAt": datetime.now(UTC) - timedelta(days=days_ago),
    }
    _, ref = get_db().collection("devices").document(device_id).collection("locations").add(doc)
    return ref.id


def _insert_stale_loc_wire_id(loc_id: str, device_id: str, *, days_ago: float) -> None:
    get_db().collection("locWireIds").document(loc_id).set(
        {"deviceId": device_id, "createdAt": datetime.now(UTC) - timedelta(days=days_ago)}
    )


def _insert_stale_loc_req(device_id: str, *, days_ago: float) -> None:
    get_db().collection("locReqs").document(device_id).set(
        {
            "messageId": "m_stale",
            "requesterUids": ["x"],
            "createdAt": datetime.now(UTC) - timedelta(days=days_ago),
        }
    )


def test_sweep_deletes_messages_and_their_wire_ids_past_retention():
    settings_store.set_retention(
        messages=RetentionSetting(n=2, unit="weeks"), locations=RetentionSetting(n=1, unit="weeks")
    )
    stale_id = _insert_stale_message("a", "b", days_ago=15, wire_id="w_stale")
    fresh_id = _insert_stale_message("a", "b", days_ago=1, wire_id="w_fresh")

    result = jobs.sweep()

    assert messages_store.get_message(stale_id) is None
    assert messages_store.get_message(fresh_id) is not None
    assert not get_db().collection("wireIds").document("w_stale_b").get().exists
    assert get_db().collection("wireIds").document("w_fresh_b").get().exists
    assert result.messagesDeleted == 1
    assert result.wireIdsDeleted == 1


def test_sweep_deletes_orphaned_wire_ids_independently_of_their_message():
    """M1 (build review, phase 6): a `wireIds` doc whose parent `messages`
    doc was deleted through some *other* path (simulating what a future
    Phase 8 user-deletion pass will do -- delete the message directly,
    without going through this sweep's paired message+wireIds delete) must
    still get reclaimed, once its own `createdAt` is past the messages
    retention cutoff, by the independent `wireIds` sweep pass rather than
    being orphaned forever."""
    settings_store.set_retention(
        messages=RetentionSetting(n=2, unit="weeks"), locations=RetentionSetting(n=1, unit="weeks")
    )
    msg = messages_store.create_message(
        sender_uid="orphan-a", recipient_uid="orphan-b", kind="text", ts=1000, body="hi",
        wire_id="w_orphan",
    )
    assert msg is not None
    wire_ref = get_db().collection("wireIds").document("w_orphan_orphan-b")
    assert wire_ref.get().exists

    # Simulate a future user-deletion pass: remove just the message doc,
    # leaving the wireIds doc with no parent message to be swept alongside.
    get_db().collection("messages").document(msg.id).delete()
    # Backdate the orphan past the messages retention cutoff -- there is no
    # store-layer API for this (see this module's docstring), same whitebox
    # approach every other sweep test here uses.
    wire_ref.update({"createdAt": datetime.now(UTC) - timedelta(days=15)})

    result = jobs.sweep()

    assert not wire_ref.get().exists
    assert result.orphanedWireIdsDeleted == 1
    # Not double-counted by the message-paired pass -- the message was
    # already gone by the time sweep() ran, so that pass never saw it.
    assert result.wireIdsDeleted == 0


def test_sweep_deletes_conversations_past_retention():
    """M2 (build review, phase 6): a `conversations/{convKey}` summary doc
    (`lastPreview`, `unread`) must not survive its thread's messages once
    the configured retention period has passed -- swept by `lastMessageAt`
    against the same `retention.messages` cutoff messages themselves use."""
    settings_store.set_retention(
        messages=RetentionSetting(n=2, unit="weeks"), locations=RetentionSetting(n=1, unit="weeks")
    )
    key = messages_store.conv_key("conv-a", "conv-b")
    _insert_stale_message("conv-a", "conv-b", days_ago=15)
    # No store-layer API backdates a conversation's lastMessageAt (every
    # real write path stamps SERVER_TIMESTAMP) -- write it directly, the
    # same whitebox approach _insert_stale_message uses for messages.
    get_db().collection("conversations").document(key).set(
        {
            "uids": sorted(["conv-a", "conv-b"]),
            "lastMessageAt": datetime.now(UTC) - timedelta(days=15),
            "lastPreview": "stale preview text",
            "unread": {"conv-b": 1},
        }
    )
    assert messages_store.get_conversation(key) is not None

    result = jobs.sweep()

    assert messages_store.get_conversation(key) is None
    assert result.conversationsDeleted == 1


def test_sweep_batches_correctly_with_a_small_sweep_batch(monkeypatch):
    """`SWEEP_BATCH=2` forces multiple internal query/delete iterations for
    5 stale messages -- `sweep()`'s own loop must still reach every one of
    them in a single call (the "loop until empty" mechanics, §5.7), not just
    the first page."""
    monkeypatch.setenv("SWEEP_BATCH", "2")
    settings_store.set_retention(
        messages=RetentionSetting(n=1, unit="days"), locations=RetentionSetting(n=1, unit="days")
    )
    ids = [_insert_stale_message("a", "b", days_ago=5) for _ in range(5)]

    result = jobs.sweep()

    assert result.messagesDeleted == 5
    for mid in ids:
        assert messages_store.get_message(mid) is None


def test_sweep_is_idempotent_across_two_consecutive_calls(monkeypatch):
    """Resumability variant this phase's brief allows when a forced mid-run
    abort can't be cleanly simulated (see `tools/e2e_v2.py`'s
    `scenario_retention` docstring for the integration-level version of this
    same check): with a small `SWEEP_BATCH` and more than one batch's worth
    of stale documents, the first `sweep()` call reaches full completion and
    the second is a safe no-op -- both properties together are what
    "idempotent and resumable" (§5.7) requires."""
    monkeypatch.setenv("SWEEP_BATCH", "2")
    settings_store.set_retention(
        messages=RetentionSetting(n=1, unit="days"), locations=RetentionSetting(n=1, unit="days")
    )
    for _ in range(5):
        _insert_stale_message("a", "b", days_ago=5)

    first = jobs.sweep()
    second = jobs.sweep()

    assert first.messagesDeleted == 5
    assert second.messagesDeleted == 0
    assert second.wireIdsDeleted == 0


def test_sweep_respects_days_vs_weeks_units_independently_per_class():
    settings_store.set_retention(
        messages=RetentionSetting(n=1, unit="days"), locations=RetentionSetting(n=1, unit="weeks")
    )
    stale_by_1_day = _insert_stale_message("a", "b", days_ago=2)
    within_1_week = _insert_stale_location("dev-units", days_ago=2)

    result = jobs.sweep()

    assert messages_store.get_message(stale_by_1_day) is None
    assert result.messagesDeleted == 1
    remaining_ids = {f.id for f in locations_store.list_locations("dev-units", limit=10)}
    assert within_1_week in remaining_ids
    assert result.locationsDeleted == 0


def test_sweep_deletes_locations_loc_wire_ids_and_loc_reqs_together():
    settings_store.set_retention(
        messages=RetentionSetting(n=4, unit="weeks"), locations=RetentionSetting(n=1, unit="days")
    )
    stale_loc = _insert_stale_location("dev-loc-sweep", days_ago=3)
    fresh_loc = _insert_stale_location("dev-loc-sweep", days_ago=0.01)
    _insert_stale_loc_wire_id("l_stale1", "dev-loc-sweep", days_ago=3)
    _insert_stale_loc_req("dev-loc-sweep", days_ago=3)

    result = jobs.sweep()

    remaining_ids = {f.id for f in locations_store.list_locations("dev-loc-sweep", limit=10)}
    assert stale_loc not in remaining_ids
    assert fresh_loc in remaining_ids
    assert not get_db().collection("locWireIds").document("l_stale1").get().exists
    assert not get_db().collection("locReqs").document("dev-loc-sweep").get().exists
    assert result.locationsDeleted == 1
    assert result.locWireIdsDeleted == 1
    assert result.locReqsDeleted == 1


def test_sweep_leaves_fresh_documents_untouched():
    settings_store.set_retention(
        messages=RetentionSetting(n=4, unit="weeks"), locations=RetentionSetting(n=1, unit="weeks")
    )
    fresh_msg = _insert_stale_message("a", "b", days_ago=0.01)
    fresh_loc = _insert_stale_location("dev-fresh", days_ago=0.01)

    result = jobs.sweep()

    assert messages_store.get_message(fresh_msg) is not None
    remaining_ids = {f.id for f in locations_store.list_locations("dev-fresh", limit=10)}
    assert fresh_loc in remaining_ids
    assert result.messagesDeleted == 0
    assert result.locationsDeleted == 0


def test_sweep_marks_settings_meta_last_swept_at():
    settings_store.set_retention(
        messages=RetentionSetting(n=4, unit="weeks"), locations=RetentionSetting(n=1, unit="weeks")
    )
    before = settings_store.get_meta().lastSweepAt
    assert before is None

    jobs.sweep()

    after = settings_store.get_meta().lastSweepAt
    assert after is not None

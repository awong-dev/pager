"""`app.jobs.tick()`: retries pager deliveries still 'queued', at most 10 per
device, oldest first -- docs/SERVER_PLAN.md §5.8 item 1. Uses
`app.tasks.InlineTaskQueue` (the only mode this phase implements) so retries
run synchronously and are observable immediately.
"""

from __future__ import annotations

import json

from app import jobs
from app.routing import Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
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

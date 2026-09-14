"""`/internal/tick`'s scheduled work -- docs/SERVER_PLAN.md §5.8.

Phase 3 implements item 1 only:

1. Retry pager deliveries still `queued` (their broker publish never got a
   2xx) -- at most 10 per device, oldest first, the same cap/ordering as the
   online-edge republish (both come from `messages_store.
   list_pending_for_device`).

Items 2 (clear `locReqs/{d}` older than 15 min) and 3 (drop push tokens past
their error threshold) are Phase 4 (location) and Phase 5/8 (retention)
work respectively -- there is nothing yet for either to do (no `locReqs`
collection exists before Phase 4; no code increments a push token's
`errorCount` before a real FCM client exists in Phase 6), so they are left
out rather than stubbed as no-ops that would need to be remembered later.

This is what finally gives `app/store/legacy.py`'s TODO-flagged
`retry_queued` a real sibling for the v2 model -- see `app/ingest.py`'s
`retry_queued` docstring for why that method stays rather than being
deleted (it retries a different, still-live storage model, the legacy
per-device thread, that `tick()` has no equivalent of).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass

from app.routing import Routing
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.tasks import TaskQueue, build_task_queue

logger = logging.getLogger("relay.jobs")


@dataclass(frozen=True, slots=True)
class TickResult:
    devicesChecked: int
    retriesAttempted: int


def tick(routing: Routing, *, task_queue: TaskQueue | None = None) -> TickResult:
    task_queue = task_queue if task_queue is not None else build_task_queue()
    devices_checked = 0
    retries_attempted = 0

    for device in devices_store.list_devices():
        if device.revokedAt is not None:
            continue
        devices_checked += 1

        for msg in messages_store.list_pending_for_device(device.id):
            bid = messages_store.find_pager_delivery(msg, device.id)
            if bid is None:
                continue
            delivery = msg.deliveries.get(bid)
            if delivery is None or delivery.state != "queued":
                # 'sent' (broker accepted it, no ack yet) is the online
                # edge's job (PROTOCOL.md §5.3), not tick's -- only a
                # publish that never even reached the broker belongs here
                # (§5.8 item 1: "their broker publish never got a 2xx").
                continue
            retries_attempted += 1
            task_queue.enqueue(
                lambda msg=msg, device_id=device.id: routing.redeliver_pager(msg, device_id),
                name=f"pager-retry:{msg.id}:{device.id}",
            )

    return TickResult(devicesChecked=devices_checked, retriesAttempted=retries_attempted)

"""`/internal/tick`'s and `/internal/sweep`'s scheduled work --
docs/SERVER_PLAN.md §5.7, §5.8.

## `tick()` (§5.8, every 5 min)

1. Retry pager deliveries still `queued` (their broker publish never got a
   2xx) -- at most 10 per device, oldest first, the same cap/ordering as the
   online-edge republish (both come from `messages_store.
   list_pending_for_device`).
1b. **(Phase 5 addition, beyond §5.8's literal text)** Retry *non-pager*
   backend deliveries still `queued` -- today just `sms`
   (`app/backends/sms_stub.py`), the first non-pager adapter that can
   actually queue/fail. §5.8 item 1 as written only names pager retries
   (the only backend that existed when it was written); §5.2's general rule
   ("Any failure ... enqueues a retry ... max 5 -> failed") is not scoped to
   any one kind, so this closes that gap the smallest way that fits the
   existing architecture: `messages_store.list_recent_queued_by_kind` scans
   recently-created messages (bounded, same `createdAt` index the pager
   query already uses) for a still-`queued` delivery of a given kind, and
   `Routing.redeliver` (the generic sibling of `redeliver_pager`, which
   stays for its device-id-keyed fast path) re-invokes that one backend's
   `deliver()`. `tools/e2e_v2.py`'s `fanout` scenario is what this addition
   exists to make pass (docs/SERVER_PLAN.md §8 scenario 7's "confirm the sms
   delivery goes through the retry path ... reaches 'failed' after the
   attempts cap" has no other mechanism to attach to, since sms deliveries
   carry no device id and never populate `pendingDeviceIds`). A real
   per-adapter Cloud Tasks enqueue-at-send-time (rather than this
   scan-on-tick fallback) is still Phase 7/8 scope, per `app/tasks.py`'s
   docstring -- this is deliberately the smallest addition that gives the
   phase's own new backend a working retry path, not a rewrite of the retry
   architecture.
2. Clear `locReqs/{d}` documents older than `app.location.loc_req_ttl_s()`
   (Phase 4, PROTOCOL.md §13.4) -- `app.location.clear_stale_loc_reqs`.

Item 3 (drop push tokens past their error threshold) is Phase 5/8
(retention) work -- there is nothing yet for it to do (no code increments a
push token's `errorCount` before a real FCM client exists in Phase 6), so
it is left out rather than stubbed as a no-op that would need to be
remembered later.

This is what finally gives `app/store/legacy.py`'s TODO-flagged
`retry_queued` a real sibling for the v2 model -- see `app/ingest.py`'s
`retry_queued` docstring for why that method stays rather than being
deleted (it retries a different, still-live storage model, the legacy
per-device thread, that `tick()` has no equivalent of).

## `sweep()` (§5.7, weekly via Cloud Scheduler, or `POST /internal/sweep`
   on demand)

Retention settings are a count + unit (`{n, unit: 'days'|'weeks'}`,
`app/store/settings.py`), converted to seconds only here. Six collections
are swept, each by its own `createdAt` (or, for `conversations`,
`lastMessageAt`) field against its own class's cutoff (messages/wireIds/
conversations use `retention.messages`; locations/locWireIds/locReqs use
`retention.locations` -- `(build finding, phase 4 review)` flagged that
`locWireIds` needs sweeping alongside `locations` or it grows unbounded, and
`locReqs` similarly has no other eventual cleanup once `app.location.
clear_stale_loc_reqs`'s much shorter 15-minute TTL has already fired):

- `messages` (+ their `wireIds/{wireId}_{recipientUid}` companion, looked up
  from the message doc's own `wireId`/`recipientUid` fields rather than a
  second query).
- `wireIds`, independently, by its own `createdAt` (`(build finding, phase 6
  review)` M1: today every `wireIds` doc is only ever reachable through its
  parent message's `wireId`/`recipientUid` fields, so a future deletion path
  that removes the message *without* going through this sweep's paired
  delete above -- Phase 8's "deleting a user removes every document keyed
  by their UID" -- would otherwise orphan it permanently. This pass is pure
  hygiene/bounded-growth, not a dedup-safety fix: an orphaned `wireIds` doc
  is inert garbage either way, never re-read once its message is gone. Never
  double-counts against the paired pass above -- a `wireIds` doc shares its
  parent message's `createdAt` exactly, since both are written in the same
  transaction, so by the time this pass's query runs, anything the paired
  pass already deleted this same call is already gone).
- `locations` (a `COLLECTION_GROUP` query across every `devices/{d}/
  locations` subcollection).
- `locWireIds` (the `/loc` dedup marker, keyed by the wire envelope's own
  `id`, not the `locations` doc's autoid -- see `app/location.py`'s module
  docstring for why it can't be joined to a `locations` doc by a shared key,
  so it is swept independently by its own `createdAt`).
- `locReqs` (a longer-lived safety net alongside `clear_stale_loc_reqs`'s
  15-minute TTL).
- `conversations`, by `lastMessageAt` (`(build finding, phase 6 review)` M2:
  without this, a `conversations/{convKey}` summary -- `lastPreview`, a
  truncated message body, and `unread` counts, both readable by either
  participant per `firestore.rules` -- outlives every message in its thread
  once the `messages` pass above has swept them all, which is chat content
  surviving its configured retention period. Uses the same `retention.
  messages` cutoff messages themselves use, since a conversation summarizes
  messages rather than having a retention class of its own).

**Mechanics** (§5.7: "`messages where createdAt < cutoff order by createdAt
limit 500`, ... deletes ... loop until empty"): each collection is swept by
`_sweep_by_created_at`, a `while True: query.limit(SWEEP_BATCH).stream();
if empty: break; delete the page; if page shorter than SWEEP_BATCH: break`
loop. **Chosen over `BulkWriter`** (which §5.7 names as an alternative):
`BulkWriter` streams writes asynchronously in the background and only
exposes completion via a `close()`/callback, which makes "run the loop
`SWEEP_BATCH` docs at a time and let a test observe each page" harder to
reason about deterministically than a plain synchronous `WriteBatch` per
page -- and a plain batch is exactly what this household-scale workload
(§9.3: low thousands of ops/day) needs. Writes within a page are chunked at
Firestore's 500-writes-per-`WriteBatch` limit by `_AutoBatch` below
(independent of `SWEEP_BATCH`, the *query* page size -- a messages page
that pairs every message with a `wireIds` delete can produce up to
`2 * SWEEP_BATCH` writes, which must not itself exceed 500).

**Idempotent and resumable** (§5.7): every query is always "everything
still older than cutoff", re-evaluated fresh on every call -- there is no
resume token, no run-id, no "in progress" flag. A sweep that never got to
finish (a Cloud Run request timeout mid-loop; a crash) leaves some matching
documents behind, and the *next* call (the following week's Cloud Scheduler
run, or a manual `POST /internal/sweep`) finds and deletes exactly those,
because the query it runs is the same query, evaluated at a fresh "now".
Calling `sweep()` twice in a row when there is nothing left over from the
first call is therefore always safe: the second call's queries return
empty and every counter in its `SweepResult` is 0.
"""

from __future__ import annotations

import logging
import os
import time
from dataclasses import dataclass
from datetime import UTC, datetime

from google.cloud.firestore import (
    Client,
    CollectionGroup,
    FieldFilter,
    Query,
    WriteBatch,
)

from app import location
from app.db.firestore import get_db
from app.routing import Routing
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import settings as settings_store
from app.store.settings import RetentionSetting
from app.tasks import TaskQueue, build_task_queue

logger = logging.getLogger("relay.jobs")

# docs/SERVER_PLAN.md §5.8: "at most 10 per device" for pager retries.
NON_PAGER_RETRY_KINDS: tuple[str, ...] = ("sms",)
NON_PAGER_RETRY_SCAN_LIMIT = 50
# (Phase 6 M3 fix, build review) mirrors §5.8 item 1's own "at most 10 per
# device" pager-retry cap -- caps how many non-pager retries `tick()`
# actually *dispatches* (not scans; `NON_PAGER_RETRY_SCAN_LIMIT` above still
# bounds the read) per call, so a broker-and-Twilio-mock-both-down tick
# can't approach Cloud Run's request timeout by inline-running up to
# `NON_PAGER_RETRY_SCAN_LIMIT` * `sms_stub.REQUEST_TIMEOUT_S` (~250s) worth
# of blocking HTTP calls. Same order of magnitude, same reasoning as §5.8's
# existing cap; a straggler beyond the cap is just retried on the *next*
# tick, 5 minutes later -- no different from today's pager cap already
# working that way.
NON_PAGER_RETRY_DISPATCH_LIMIT = 10


@dataclass(frozen=True, slots=True)
class TickResult:
    devicesChecked: int
    retriesAttempted: int
    locReqsCleared: int = 0
    nonPagerRetriesAttempted: int = 0


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

    non_pager_retries_attempted = 0
    for kind in NON_PAGER_RETRY_KINDS:
        if non_pager_retries_attempted >= NON_PAGER_RETRY_DISPATCH_LIMIT:
            break
        for msg in messages_store.list_recent_queued_by_kind(kind, limit=NON_PAGER_RETRY_SCAN_LIMIT):
            if non_pager_retries_attempted >= NON_PAGER_RETRY_DISPATCH_LIMIT:
                break
            for bid, delivery in msg.deliveries.items():
                if delivery.kind != kind or delivery.state != "queued":
                    continue
                if non_pager_retries_attempted >= NON_PAGER_RETRY_DISPATCH_LIMIT:
                    break
                non_pager_retries_attempted += 1
                task_queue.enqueue(
                    lambda msg=msg, bid=bid: routing.redeliver(msg, bid),
                    name=f"{kind}-retry:{msg.id}:{bid}",
                )

    loc_reqs_cleared = location.clear_stale_loc_reqs()

    return TickResult(
        devicesChecked=devices_checked,
        retriesAttempted=retries_attempted,
        locReqsCleared=loc_reqs_cleared,
        nonPagerRetriesAttempted=non_pager_retries_attempted,
    )


# ---------------------------------------------------------------------------
# sweep() -- docs/SERVER_PLAN.md §5.7
# ---------------------------------------------------------------------------

DEFAULT_SWEEP_BATCH = 500
# Firestore's own hard cap on writes in one `WriteBatch.commit()`.
MAX_BATCH_WRITES = 500


def _sweep_batch_size() -> int:
    """Query page size, per collection, per iteration of a sweep's inner
    loop -- overridable via `SWEEP_BATCH` (read fresh on every `sweep()`
    call, the same per-call `os.environ.get(...)` pattern
    `app.location.loc_req_ttl_s()` uses, so a test can shrink it without
    reloading this module) so a test can force multiple iterations without
    creating hundreds of documents."""
    return int(os.environ.get("SWEEP_BATCH", str(DEFAULT_SWEEP_BATCH)))


def _retention_seconds(setting: RetentionSetting) -> int:
    if setting.unit == "days":
        return setting.n * 86400
    return setting.n * 7 * 86400


def _cutoff(seconds: int) -> datetime:
    return datetime.fromtimestamp(time.time() - seconds, tz=UTC)


class _AutoBatch:
    """Wraps a `WriteBatch`, auto-committing and starting a fresh one every
    `MAX_BATCH_WRITES` deletes -- independent of `SWEEP_BATCH` (the *query*
    page size) so a page that pairs every message with a `wireIds` delete
    (up to `2 * SWEEP_BATCH` writes) never exceeds Firestore's own 500-op
    limit on a single batch, regardless of how large `SWEEP_BATCH` is set."""

    def __init__(self, db: Client) -> None:
        self._db = db
        self._batch: WriteBatch = db.batch()
        self._count = 0

    def reserve(self, n: int) -> None:
        """Commit the current batch *now* if the next `n` deletes would not
        all fit in it -- so a caller with a group of deletes that must land
        together (a message and its `wireIds` companion) never has that
        group split across two commits. Without this, `delete()`'s own
        flush-at-500 could commit the message delete and leave the
        companion for the next batch; a Cloud Run request timeout in that
        window (the exact interruption §5.7 anticipates) would orphan a
        `wireIds` document *permanently*, because `wireIds` docs carry no
        `createdAt` of their own and are only ever reachable through the
        `wireId`/`recipientUid` fields of the message that just got
        deleted."""
        if self._count and self._count + n > MAX_BATCH_WRITES:
            self._batch.commit()
            self._batch = self._db.batch()
            self._count = 0

    def delete(self, ref) -> None:
        self._batch.delete(ref)
        self._count += 1
        if self._count >= MAX_BATCH_WRITES:
            self._batch.commit()
            self._batch = self._db.batch()
            self._count = 0

    def commit(self) -> None:
        if self._count:
            self._batch.commit()
            self._batch = self._db.batch()
            self._count = 0


def _sweep_by_created_at(
    query_factory,
    cutoff: datetime,
    batch_size: int,
    *,
    on_doc=None,
    created_at_field: str = "createdAt",
) -> int:
    """Generic "delete every doc older than `cutoff`" loop, shared by every
    collection this sweep touches (see this module's docstring for why a
    plain `WriteBatch` loop was chosen over `BulkWriter`). `query_factory()`
    returns a fresh, unfiltered `Query`/`CollectionGroup`/`CollectionReference`
    each call (a Firestore `Query` object is immutable and side-effect-free
    to build, so rebuilding it every iteration is cheap and avoids any
    "did I already apply .limit() twice" bookkeeping). `on_doc(batch, snap)`,
    if given, lets a caller stage extra deletes (the `wireIds` companion)
    alongside each matched doc's own delete, in the *same* `WriteBatch`
    commit (`_AutoBatch.reserve` below guarantees the pair is never split
    across two commits, so a crash or a Cloud Run timeout between the two is
    never possible -- both commit together or neither does). `created_at_field`
    (Phase 6 M2 fix) lets `conversations` reuse this same loop against its own
    `lastMessageAt` field rather than `createdAt` -- a conversation summary
    has no `createdAt` of its own, only the timestamp of its most recent
    message."""
    db = get_db()
    deleted = 0
    while True:
        query: Query | CollectionGroup = (
            query_factory()
            .where(filter=FieldFilter(created_at_field, "<", cutoff))
            .order_by(created_at_field)
            .limit(batch_size)
        )
        docs = list(query.stream())
        if not docs:
            break
        batch = _AutoBatch(db)
        group = 1 if on_doc is None else 2
        for snap in docs:
            # Keep each doc and its companion in one commit -- see
            # `_AutoBatch.reserve`.
            batch.reserve(group)
            batch.delete(snap.reference)
            deleted += 1
            if on_doc is not None:
                on_doc(batch, snap)
        batch.commit()
        if len(docs) < batch_size:
            # Fewer than a full page came back -- this class is exhausted;
            # save one more (empty) round-trip next loop iteration.
            break
    return deleted


@dataclass(frozen=True, slots=True)
class SweepResult:
    messagesDeleted: int
    wireIdsDeleted: int
    locationsDeleted: int
    locWireIdsDeleted: int
    locReqsDeleted: int
    orphanedWireIdsDeleted: int = 0
    conversationsDeleted: int = 0


def sweep() -> SweepResult:
    db = get_db()
    retention = settings_store.get_retention()
    batch_size = _sweep_batch_size()
    msg_cutoff = _cutoff(_retention_seconds(retention.messages))
    loc_cutoff = _cutoff(_retention_seconds(retention.locations))

    wire_ids_deleted = 0

    def _delete_companion_wire_id(batch: _AutoBatch, snap) -> None:
        nonlocal wire_ids_deleted
        data = snap.to_dict() or {}
        wire_id = data.get("wireId")
        recipient_uid = data.get("recipientUid")
        if wire_id and recipient_uid:
            batch.delete(db.collection("wireIds").document(f"{wire_id}_{recipient_uid}"))
            wire_ids_deleted += 1

    messages_deleted = _sweep_by_created_at(
        lambda: db.collection("messages"),
        msg_cutoff,
        batch_size,
        on_doc=_delete_companion_wire_id,
    )

    locations_deleted = _sweep_by_created_at(
        lambda: db.collection_group("locations"), loc_cutoff, batch_size
    )
    loc_wire_ids_deleted = _sweep_by_created_at(
        lambda: db.collection("locWireIds"), loc_cutoff, batch_size
    )
    loc_reqs_deleted = _sweep_by_created_at(
        lambda: db.collection("locReqs"), loc_cutoff, batch_size
    )
    # (Phase 6 M1 fix, build review) `wireIds` docs no longer reachable only
    # as a side effect of deleting their parent `messages` doc (see
    # `create_message`'s `createdAt` write) -- swept independently, by the
    # same `retention.messages` cutoff, so a doc orphaned by any other
    # deletion path (today: none; Phase 8's future user-deletion pass is the
    # motivating case) still gets reclaimed on its own schedule instead of
    # growing unbounded. Whatever the message-paired pass above already
    # deleted in *this* call is gone by the time this query runs, so the two
    # counts never double-count the same document.
    orphaned_wire_ids_deleted = _sweep_by_created_at(
        lambda: db.collection("wireIds"), msg_cutoff, batch_size
    )
    # (Phase 6 M2 fix, build review) `conversations/{convKey}` summary docs
    # (`lastPreview`, a truncated message body; `unread` counts) otherwise
    # outlive every message in their thread once `messages` above has swept
    # them all -- chat content surviving its configured retention period.
    # Swept by `lastMessageAt` against the same `retention.messages` cutoff
    # messages themselves use (a conversation summarizes messages, so it
    # inherits their retention class rather than getting a setting of its
    # own).
    conversations_deleted = _sweep_by_created_at(
        lambda: db.collection("conversations"), msg_cutoff, batch_size, created_at_field="lastMessageAt"
    )

    settings_store.mark_swept()

    logger.info(
        "sweep complete: messages=%d wireIds=%d locations=%d locWireIds=%d locReqs=%d "
        "orphanedWireIds=%d conversations=%d",
        messages_deleted,
        wire_ids_deleted,
        locations_deleted,
        loc_wire_ids_deleted,
        loc_reqs_deleted,
        orphaned_wire_ids_deleted,
        conversations_deleted,
    )
    return SweepResult(
        messagesDeleted=messages_deleted,
        wireIdsDeleted=wire_ids_deleted,
        locationsDeleted=locations_deleted,
        locWireIdsDeleted=loc_wire_ids_deleted,
        locReqsDeleted=loc_reqs_deleted,
        orphanedWireIdsDeleted=orphaned_wire_ids_deleted,
        conversationsDeleted=conversations_deleted,
    )

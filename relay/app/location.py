"""Device location -- docs/PROTOCOL.md §13, docs/SERVER_PLAN.md §5.6.

Two independent things live here:

1. **`/loc` ingest** (`ingest_loc`, called by `app/ingest.py`'s webhook
   dispatch once it has validated the envelope shape via `wire.LocEnvelope`):
   dedups on the envelope's own `id` (§13.2 -- "exactly as §4.2 dedups an up
   message, in the same transaction that stores the fix"), writes to
   `devices/{deviceId}/locations/{autoId}` when the envelope carries a real
   fix, and -- when `req` is set -- resolves the matching `loc_req` (marks
   its pager delivery `fulfilled`, deletes `locReqs/{deviceId}`, and posts
   one `kind='loc'` thread message per coalesced requester), **all in the
   same single transaction as the dedup marker and the fix write** (§5.6,
   and PROTOCOL.md §13.2's "in the same transaction" taken literally).
   Splitting dedup and fulfilment across two transactions leaves a window
   where a crash or timeout between them makes a webhook redelivery see the
   dedup marker already committed and silently swallow the retry, leaving
   the `loc_req` stuck `sent` forever with no requester ever notified. A
   periodic fix (`req: null`) is never a thread message, per §3.2/§5.6 --
   only the `locations` collection entry.

2. **`/locate`** (`Location.locate`, called by
   `app/routers/conversations.py`'s `POST /api/conversations/{alias}/locate`):
   one transaction on `locReqs/{deviceId}` implementing PROTOCOL.md §13.3
   rules 5-7 -- coalesce onto a live (<15 min) request, answer from a
   <60s-old cached fix with no wire traffic at all, or claim the slot and
   create a fresh `loc_req` message, delivered inline through the same
   `Routing.redeliver_pager` primitive the online-edge republish and
   `/internal/tick` retries use.

**Schema footnote**: §3's table
does not list a dedup collection for `/loc` envelopes the way it lists
`wireIds/{wireId}_{recipientUid}` for up messages. `locWireIds/{deviceId}_{id}`
below is the same *pattern* (a `transaction.create()`-or-`AlreadyExists` dedup
marker, one field: `deviceId`) applied to `/loc`'s own id space (`l_` + 8
hex, §1) instead of reusing `wireIds`, kept as a separate collection because
the two dedup keys are shaped differently (`wireIds` docs are keyed
`{wireId}_{recipientUid}`; a `/loc` fix has no recipient to key against).
This is additive to §3, not a change to it.

**Dedup key, revised (docs/LOCATION_TRACKING_DESIGN.md §5 R8):** keyed
`{deviceId}_{id}`, not bare `id` -- `id` is only 32 random bits
(firmware `loc.c`'s id generator) and this collection is shared by every
device, so two devices' reports can collide on the same `id` and silently
drop one of them. `app/jobs.py`'s sweep goes by `createdAt`, so it needs no
change for this. A duplicate that straddles the deploy of this change may
be stored twice, once under each key shape -- acceptable, no migration.

**`loc_req` is device-targeted, not user-targeted** -- which is why
`Location.locate` does not go through
`app.routing.Routing.send()`. Unlike an ordinary text send, which fans a
message out to every enabled backend of the *resolved user*
(`app/routing.py`'s `send()`), a location request must reach exactly the
one device being asked about -- fanning it out to the owner's other
backends (`webapp`, `sms`, ...) makes no sense (there's nothing for those
backends to *do* with a location request). So this module builds the
`messages/{id}` document directly, with a single `pager`-kind delivery
entry addressed at that one device, inside its own coalescing transaction.
The *inline delivery* step (`BrokerClient.publish` + attempts/error/
`pendingDeviceIds` bookkeeping) still reuses `Routing.redeliver_pager` --
the same primitive the online-edge republish and `/internal/tick` retries
call -- so a `loc_req`'s delivery accounting is identical to every other
pager delivery's.
"""

from __future__ import annotations

import logging
import os
import random
import time
from dataclasses import dataclass
from datetime import UTC, datetime

from google.api_core.exceptions import Aborted, AlreadyExists
from google.cloud.firestore import (
    SERVER_TIMESTAMP,
    ArrayRemove,
    ArrayUnion,
    DocumentReference,
    DocumentSnapshot,
    FieldFilter,
    Transaction,
)

from app import cellgeo
from app.db.firestore import get_db, run_transaction
from app.ids import new_id
from app.routing import Routing
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import locations as locations_store
from app.store import messages as messages_store
from app.store.backends import Backend as BackendRow
from app.store.locations import LocationFix
from app.store.messages import Message
from app.wire import CellInfo, LocEnvelope, LocFix, resolve_ts

logger = logging.getLogger("relay.location")

# PROTOCOL.md §13.4 / §3.2: "the expiry is 15 minutes after creation".
DEFAULT_LOC_REQ_TTL_S = 15 * 60
# PROTOCOL.md §13.3 rule 6: "A request arriving within 60s of a fulfilled
# one is answered from the stored fix with cached:true, without any wire
# traffic to the device at all." docs/SERVER_PLAN.md §5.6 generalises this
# to "a fix < 60s old exists in devices/{deviceId}/locations" (periodic or
# on-demand, not just one that answered a request) -- this module follows
# that (more precise, implementation-facing) wording.
CACHED_ANSWER_WINDOW_S = 60

# docs/LOCATION_TRACKING_DESIGN.md §5 R6 (this task): the relay's own mirror
# of PROTOCOL.md §13.3 item 9's 120s device-side floor on unsolicited `/loc`
# reports -- bounds how often a buggy or flooding device can make this relay
# call the cell-geo provider and write a new `locations` doc. A distinct
# concept from `CACHED_ANSWER_WINDOW_S` above (that one bounds how fresh a
# fix must be to answer a `/locate` with no wire trip; this one bounds
# ingest's own provider-call/write rate) that happens to share the same
# numeric value, so kept as its own name rather than reusing that constant.
UNSOLICITED_REPORT_FLOOR_S = 60

# docs/LOCATION_TRACKING_DESIGN.md §5 R3 (this task): a dwell doc (an
# unsolicited, `src:"cell"` fix extended in place by later reports from the
# same cell, rather than a fresh doc per report) stops extending once its
# `createdAt` is this old -- `app/jobs.py`'s sweep deletes `locations` by
# `createdAt`, so an unbounded dwell doc would otherwise be swept out from
# under a device that never changes cell, while it is still the newest point
# on the map.
DWELL_MAX_AGE_S = 24 * 60 * 60

# PROTOCOL.md §13.3 rule 5: under heavy write contention on a single
# device's `locReqs/{deviceId}` doc (this module's own concurrency test
# reproduces it with N threads racing one un-claimed device), the
# google-cloud-firestore client's own transactional retry budget (5 attempts
# per `db.transaction()` object), chained through `run_transaction`'s own
# outer retries (`app/db/firestore.py`'s `EXTRA_RETRY_ATTEMPTS`), can still
# be exhausted on a slow/loaded emulator: `run_transaction` then re-raises
# the client library's own `ValueError("Failed to commit transaction in N
# attempts.")`, chaining the losing `Aborted` as `__cause__`. A loser here is
# not a bug. Aborts (emulator lock timeouts included) do not prove another
# transaction has already committed: on a CPU-starved emulator the eventual
# winner can itself still be retrying when the losers give up, and the doc
# was observed appearing only several re-reads (seconds) later. So the right
# outcome is the same one the `AlreadyExists` path
# below already produces: join the winner's request rather than surface an
# error to the caller. Unlike `AlreadyExists`, this cannot safely retry the
# *whole* claim transaction (that is the operation that just exhausted every
# attempt it was given); `_join_after_contention` below instead does a
# bounded number of plain (non-transactional) re-reads of `req_ref`, joining
# the winner's `requesterUids` via `ArrayUnion` -- a native atomic field
# transform that needs no transaction of its own -- as soon as the winner's
# doc is visible. Budget: 10 reads, full-jitter delay caps 0.05..1.0 s, so a
# few seconds expected and ~5.5 s worst case, bounded.
CONTENTION_REREAD_ATTEMPTS = 10
CONTENTION_REREAD_BASE_DELAY_S = 0.05
CONTENTION_REREAD_MAX_DELAY_S = 1.0


def loc_req_ttl_s() -> int:
    """15 minutes in production (PROTOCOL.md §13.4). Overridable via
    `LOC_REQ_TTL_S` (docs/SERVER_PLAN.md §8 scenario 6's shortened-TTL
    derived-expiry test) -- read fresh on every call, not cached at import,
    the same `os.environ.get(...)` pattern `app/tasks.py`'s `TASKS_MODE`
    uses, so a test can flip it between calls without reloading the
    module."""
    return int(os.environ.get("LOC_REQ_TTL_S", str(DEFAULT_LOC_REQ_TTL_S)))


def _age_seconds(dt: datetime | None) -> float:
    """`inf` for "no timestamp at all" so every freshness check below (`age
    < window`) correctly treats a missing `createdAt` as infinitely stale
    rather than crashing or being accidentally treated as fresh."""
    if dt is None:
        return float("inf")
    return (datetime.now(UTC) - dt).total_seconds()


def _loc_reqs():
    return get_db().collection("locReqs")


def _loc_wire_ids():
    return get_db().collection("locWireIds")


# ---------------------------------------------------------------------------
# /loc ingest -- PROTOCOL.md §13.2, §13.4; SERVER_PLAN.md §5.6 first bullet
# ---------------------------------------------------------------------------


def _cell_key(cell: CellInfo | None) -> str | None:
    """docs/LOCATION_TRACKING_DESIGN.md §5 R3 (this task): identifies the
    serving cell a stored fix came from, so a dwell extension (`_txn` below)
    can tell "same cell, still there" from "moved to a new cell" without
    re-resolving anything. `None` when the envelope carried no `cell` at
    all (a real GNSS fix, most commonly)."""
    if cell is None:
        return None
    return f"{cell.mcc}-{cell.mnc}-{cell.tac}-{cell.ci}"


def _fix_doc(env: LocEnvelope, loc: LocFix) -> dict:
    return {
        "ts": env.ts,
        "fixTs": loc.fix_ts,
        "lat": loc.lat,
        "lon": loc.lon,
        "accM": loc.acc,
        "src": loc.src,
        "cached": env.cached,
        "reqId": env.req,
        "createdAt": SERVER_TIMESTAMP,
        # docs/LOCATION_TRACKING_DESIGN.md §5 R3/P3 (this task): see
        # `store/locations.py`'s `LocationFix` docstring for what each of
        # these means. `lastTs` is `None` on insert -- only a dwell
        # extension (`_txn` below) ever sets it.
        "why": env.why,
        "cellKey": _cell_key(env.cell),
        "lastTs": None,
    }


def _resolve_cell_fallback(cell: CellInfo, fix_ts: int) -> LocFix | None:
    """docs/PROTOCOL.md §13.2 / this task: asks `app/cellgeo.py` to turn a
    `cell` into a position. Called by `ingest_loc` only when `env.loc is
    None` (no GNSS fix -- "the pager sends `cell` whenever it answers
    without a GNSS fix"); recording `devices/{d}.status.lastCell` is a
    separate, unconditional step in `ingest_loc` itself (§13.2: "the cell is
    only recorded" even when a GNSS fix is also present, so that must happen
    whether or not this function is even called). `fix_ts` is the envelope's
    own `ts`, or the relay's receive time when `ts` is 0 (§3.5's usual rule,
    reused here verbatim since a cell-resolved position has no
    device-reported fix time of its own to fall back on). Returns `None`
    (behave exactly like today's plain `no_fix`) when resolution fails for
    any reason -- `cellgeo.resolve` itself never raises (it catches every
    exception from the third-party HTTP call), and the `except` below is a
    second line of defence around the whole call (including its own
    Firestore cache reads/writes) so a bug anywhere in that path can never
    turn into a 500 on `POST /webhooks/mqtt`."""
    try:
        resolved = cellgeo.resolve(cell)
    except Exception as exc:  # noqa: BLE001 -- must never break /webhooks/mqtt ingest
        logger.warning("cellgeo.resolve() raised unexpectedly: %r", exc)
        resolved = None
    if resolved is None:
        return None
    try:
        return LocFix(lat=resolved.lat, lon=resolved.lon, acc=resolved.acc_m, fix_ts=fix_ts, src="cell")
    except Exception as exc:  # noqa: BLE001 -- a malformed provider response must not break
        # /webhooks/mqtt ingest -- cellgeo.resolve's own sanity check
        # (accuracy, 0,0) catches the common cases, but LocFix's own
        # stricter field bounds (e.g. a real lat/lon range) are the last
        # line of defence.
        logger.warning("cell-resolved fix failed validation, treated as unresolved: %r", exc)
        return None


def _unsolicited_report_too_recent(device_id: str) -> bool:
    """docs/LOCATION_TRACKING_DESIGN.md §5 R6 (this task): a
    **non-transactional** read (like the device lookup in `ingest_loc`
    below -- not part of any invariant the ingest transaction needs
    atomicity for) of the newest `locations` doc, consulted only when
    `ingest_loc` is about to resolve an *unsolicited* (`req: None`)
    cell-only report. `True` when that doc is itself an unsolicited
    (`reqId: None`) report less than `UNSOLICITED_REPORT_FLOOR_S` old --
    ages off `max(createdAt, lastTs)` (a dwell doc's `lastTs` moves forward
    without touching `createdAt`, so the *most recent activity* is what
    must be compared against the floor, not the doc's original insert
    time). A request answer (`reqId` set) never counts here -- it is a
    different event, not part of this relay-side unsolicited-report rate
    limit."""
    fix_query = (
        locations_store.locations_collection(device_id)
        .order_by("createdAt", direction="DESCENDING")
        .limit(1)
    )
    fix_snaps = fix_query.get()
    if not fix_snaps:
        return False
    data = fix_snaps[0].to_dict() or {}
    if data.get("reqId") is not None:
        return False
    created_age = _age_seconds(data.get("createdAt"))
    last_ts = data.get("lastTs")
    last_ts_age = (time.time() - last_ts) if isinstance(last_ts, (int, float)) else float("inf")
    return min(created_age, last_ts_age) < UNSOLICITED_REPORT_FLOOR_S


def ingest_loc(device_id: str, env: LocEnvelope) -> None:
    """§13.2: dedup on `id`, storing the fix (if any -- `env.loc` is `None`
    when the device answers `err:"no_fix"`/`"disabled"`, in which case there
    is nothing to add to `locations`, only a `loc_req` to resolve, if `req`
    is set) and resolving the matching `loc_req` (`_fulfil_loc_req_in_txn`
    below), **all in one transaction**. Splitting dedup and fulfilment
    across two transactions is not safe here: if the second failed after
    the first had already committed -- a Firestore abort, a Cloud Run
    timeout, a crash -- the webhook would return a 500, EMQX would redeliver
    the QoS-1 `/loc` message, and the dedup check on retry would see the id
    already recorded and silently swallow the redelivery. The fix would get
    stored, but the `loc_req` delivery would stay stuck `sent` (until the
    15-minute derived expiry) with no requester ever notified. Keeping
    fulfilment in the same transaction as the dedup marker means both commit
    or neither does,
    so a webhook redelivery after a partial failure retries the whole thing
    and reaches the same end state a first-try success would have. A
    periodic fix (`req: None`) that dedups clean simply updates
    `locations`; nothing else happens.

    **Cell-tower fallback (§13.2):** when the device has no GNSS
    fix but sent a `cell`, `_resolve_cell_fallback` (an HTTP call, so it must
    run *before* the transaction below, same reasoning as the device lookup)
    either produces a `LocFix` with `src:"cell"` -- treated exactly like a
    real fix for the rest of this function, stored in `locations` and used
    to fulfil `req` if set -- or `None`, in which case behaviour is
    identical to today's `no_fix` handling. "The GNSS fix wins" when both
    are present: the cell fallback is only ever attempted when `env.loc is
    None`.

    **R1 (this task):** `req_ref`/`loc_field` below are only built when
    `env.req is not None` -- an unsolicited periodic/cell report has no
    `loc_req` to resolve, so building them anyway cost every such report an
    extra transactional read of `locReqs/{device_id}` and logged a
    misleading "late /loc ... no live loc_req, dropped" for every single one
    (docs/LOCATION_TRACKING_DESIGN.md §5 R1).

    **R6 (this task):** an unsolicited cell-only report skips
    `_resolve_cell_fallback` entirely -- no provider call, no new
    `locations` doc -- when the newest doc is itself an unsolicited report
    less than `UNSOLICITED_REPORT_FLOOR_S` old (`_unsolicited_report_too_recent`
    above), mirroring PROTOCOL.md §13.3 item 9's 120s device-side floor so a
    buggy or flooding device cannot drive unbounded provider calls/writes.
    `lastCell` is still updated either way.

    **R3 dwell (this task):** an unsolicited (`req: None`), cell-resolved
    fix does not always insert a new `locations` doc -- if the newest doc is
    itself an unsolicited `src:"cell"` fix from the *same* cell and less
    than `DWELL_MAX_AGE_S` old, this extends that doc's `lastTs`/`why`
    instead, so a device sitting in one place all day leaves one dwell doc,
    not one every report."""
    dedup_ref = _loc_wire_ids().document(f"{device_id}_{env.id}")

    # The device lookup (unlike everything `_fulfil_loc_req_in_txn` reads)
    # is not folded into the transaction: it is not part of any invariant
    # this function needs atomicity for (an unknown/renamed device is a
    # logged-and-dropped case either way, same as `_find_pager_backend`'s
    # non-transactional read in `Location.locate`), and reading it inside
    # the transaction would gain nothing but an extra round trip. Looked up
    # whenever there's a `req` to resolve *or* a `cell` to record/resolve
    # (both need it -- `req` for `owner_uid`, `cell` for `set_last_cell`).
    owner_uid: str | None = None
    if env.req is not None or env.cell is not None:
        device = devices_store.get_device(device_id)
        if device is None:
            if env.req is not None:
                logger.warning("/loc req=%s for unknown device=%s dropped", env.req, device_id)
        else:
            owner_uid = device.ownerUid

    effective_loc = env.loc
    cell_fix_ts: int | None = None
    if env.cell is not None and owner_uid is not None:
        cell_fix_ts = resolve_ts(env.ts)
        if effective_loc is None:
            if env.req is None and _unsolicited_report_too_recent(device_id):
                logger.info(
                    "unsolicited /loc id=%s for device=%s within %ss of the last unsolicited "
                    "report: skipping cellgeo resolution",
                    env.id,
                    device_id,
                    UNSOLICITED_REPORT_FLOOR_S,
                )
            else:
                effective_loc = _resolve_cell_fallback(env.cell, cell_fix_ts)

    fix_ref = locations_store.new_location_ref(device_id) if effective_loc is not None else None
    fix_data = _fix_doc(env, effective_loc) if effective_loc is not None else None
    dwell_eligible = (
        fix_data is not None and env.req is None and effective_loc is not None and effective_loc.src == "cell"
    )

    # R1 (docs/LOCATION_TRACKING_DESIGN.md §5): built only when there is a
    # `loc_req` to resolve.
    req_ref = _loc_reqs().document(device_id) if owner_uid is not None and env.req is not None else None
    loc_field = (
        _loc_message_field(env, effective_loc) if owner_uid is not None and env.req is not None else None
    )
    ttl = loc_req_ttl_s()

    def _txn(transaction: Transaction) -> None:
        # -- every read this transaction needs, before any write is staged
        # (Firestore's own local rule, see app/db/firestore.py's
        # run_transaction docstring) --
        fulfil: _FulfilPlan | None = None
        drop_reason: str | None = None
        delete_stale_req = False

        # R3 dwell (docs/LOCATION_TRACKING_DESIGN.md §5, this task): same
        # query shape as `Location.locate`'s <60s cached-fix check, but
        # transactional -- reads the newest `locations` doc to decide
        # whether this report extends it (same cell, same source, still
        # unsolicited, still within the 24h dwell cap) instead of inserting
        # a fresh one.
        dwell_ref: DocumentReference | None = None
        dwell_update: dict | None = None
        if dwell_eligible:
            assert fix_data is not None
            newest_query = (
                locations_store.locations_collection(device_id)
                .order_by("createdAt", direction="DESCENDING")
                .limit(1)
            )
            newest_snaps = newest_query.get(transaction=transaction)
            if newest_snaps:
                newest_snap = newest_snaps[0]
                newest_data = newest_snap.to_dict() or {}
                if (
                    newest_data.get("src") == "cell"
                    and newest_data.get("reqId") is None
                    and newest_data.get("cellKey") == fix_data["cellKey"]
                    and _age_seconds(newest_data.get("createdAt")) < DWELL_MAX_AGE_S
                ):
                    dwell_ref = newest_snap.reference
                    dwell_update = {"lastTs": resolve_ts(env.ts), "why": env.why}

        # R7 (docs/LOCATION_TRACKING_DESIGN.md §5, this task): the read half
        # of the monotonic `lastCell` check -- must happen here, in the read
        # phase, alongside everything else this transaction reads; the
        # write half (`devices_store.set_last_cell`) is staged below with
        # every other write, using this value.
        last_cell_existing_ts: int | None = None
        if env.cell is not None and owner_uid is not None:
            last_cell_existing_ts = devices_store.read_last_cell_ts(device_id, transaction)

        if req_ref is not None:
            req_snap = req_ref.get(transaction=transaction)
            if not req_snap.exists:
                # §13.4: "a late /loc for an already-expired request is
                # logged and dropped, exactly as §4.1 rule 1 drops a
                # redundant ack" -- also covers "already fulfilled"
                # (fulfilment deletes this doc).
                drop_reason = "no live loc_req"
            else:
                req_data = req_snap.to_dict() or {}
                loc_req_msg_id = req_data.get("messageId")
                if loc_req_msg_id != env.req:
                    drop_reason = f"does not match the live loc_req {loc_req_msg_id}"
                elif _age_seconds(req_data.get("createdAt")) >= ttl:
                    # Derived-expiry (§13.4) beat this answer to the punch:
                    # drop it (never mark 'fulfilled') and clean up the
                    # now-pointless doc -- equivalent to what the next
                    # `/internal/tick` would do anyway.
                    drop_reason = f"request already past its {ttl}s TTL"
                    delete_stale_req = True
                else:
                    # An owner locating their own device (no allow edge
                    # needed, `routers/conversations.py`) gets no
                    # self-conversation `kind='loc'` message: the fix is
                    # already in `devices/{d}/locations`, which the owner
                    # can read, and a thread with oneself is not a thing.
                    requester_uids = [
                        u for u in (req_data.get("requesterUids") or []) if u != owner_uid
                    ]
                    loc_req_ref = messages_store.messages_ref(loc_req_msg_id)
                    loc_req_snap = loc_req_ref.get(transaction=transaction)
                    meta_ref = messages_store.meta_ref()
                    meta_snap = meta_ref.get(transaction=transaction)
                    conv_refs: dict[str, DocumentReference] = {}
                    conv_snaps: dict[str, DocumentSnapshot] = {}
                    # docs/FAMILIES_DESIGN.md §1 decisions 3-4: each
                    # coalesced requester gets their own `kind='loc'`
                    # message, so `familyIds`/`participants` are computed
                    # per (owner, requester) pair, not once over the whole
                    # `requester_uids` list. Users do not change during a
                    # locate, so this `users/{uid}` read needs no
                    # transactional consistency -- unlike `conv_refs`/
                    # `conv_snaps` just above, it is a **plain** (no
                    # `transaction=`) read: enrolling it in this
                    # transaction's read set would only widen the window in
                    # which a concurrent locate on the same device's
                    # `locReqs/{deviceId}` (the doc this transaction
                    # actually needs atomicity on) can abort it, without
                    # buying any invariant this function needs (see CI
                    # regression note in git history: this loop originally
                    # passed `transaction=transaction` here and exhausted
                    # `run_transaction`'s 5-attempt retry budget under
                    # N-way concurrent `/locate` contention).
                    pair_participants: dict[str, dict[str, dict[str, str]]] = {}
                    pair_family_ids: dict[str, list[str]] = {}
                    for uid in requester_uids:
                        key = messages_store.conv_key(owner_uid, uid)
                        ref = messages_store.conversation_ref(key)
                        conv_refs[uid] = ref
                        conv_snaps[uid] = ref.get(transaction=transaction)
                        participants, family_ids = messages_store.build_participants_and_family_ids(
                            [owner_uid, uid]
                        )
                        pair_participants[uid] = participants
                        pair_family_ids[uid] = family_ids
                    fulfil = _FulfilPlan(
                        loc_req_msg_id=loc_req_msg_id,
                        loc_req_ref=loc_req_ref,
                        loc_req_snap=loc_req_snap,
                        meta_ref=meta_ref,
                        meta_snap=meta_snap,
                        requester_uids=requester_uids,
                        conv_refs=conv_refs,
                        conv_snaps=conv_snaps,
                        pair_participants=pair_participants,
                        pair_family_ids=pair_family_ids,
                    )

        # -- every write from here on --

        # `transaction.create` raises AlreadyExists (caught below, outside
        # the transaction) if this `/loc` id was already processed -- same
        # idiom `app/store/messages.py`'s `create_message` uses for its
        # `wireIds` dedup.
        transaction.create(dedup_ref, {"deviceId": device_id, "createdAt": SERVER_TIMESTAMP})

        if env.cell is not None and owner_uid is not None:
            assert cell_fix_ts is not None
            devices_store.set_last_cell(
                device_id, env.cell, cell_fix_ts, transaction, existing_ts=last_cell_existing_ts
            )

        if dwell_ref is not None:
            assert dwell_update is not None
            transaction.update(dwell_ref, dwell_update)
        elif fix_ref is not None:
            transaction.set(fix_ref, fix_data)

        if req_ref is None:
            return

        if drop_reason is not None:
            logger.info(
                "late /loc id=%s req=%s for device=%s: %s, dropped",
                env.id,
                env.req,
                device_id,
                drop_reason,
            )
            if delete_stale_req:
                transaction.delete(req_ref)
            return

        assert fulfil is not None
        assert owner_uid is not None
        _fulfil_loc_req_in_txn(
            transaction,
            device_id=device_id,
            owner_uid=owner_uid,
            env=env,
            loc_field=loc_field,
            req_ref=req_ref,
            plan=fulfil,
        )

    try:
        run_transaction(_txn)
    except AlreadyExists:
        logger.debug("duplicate /loc id=%s from device=%s dropped", env.id, device_id)
        return


def _loc_message_field(env: LocEnvelope, loc: LocFix | None) -> dict:
    """The `loc` field of the `kind='loc'` thread message posted to each
    requester -- a real (or cell-resolved, `src:"cell"`) fix's shape, or
    `{"err": ...}` when neither a GNSS fix nor a resolved cell position was
    available (still worth telling the requester, rather than leaving them
    to find out only from the 15-minute `expired` derived state)."""
    if loc is not None:
        return {
            "lat": loc.lat,
            "lon": loc.lon,
            "accM": loc.acc,
            "fixTs": loc.fix_ts,
            "src": loc.src,
            "cached": env.cached,
        }
    return {"err": env.err}


def _loc_preview(loc_field: dict) -> str:
    """`loc_field` is `_loc_message_field`'s own output -- a real shape
    (including a cell-resolved one, `src:"cell"`) has no `err` key, so
    checking for that key's absence is the one source of truth for "was
    there a usable position" that already accounts for the cell fallback."""
    return "location" if "err" not in loc_field else f"location unavailable ({loc_field['err']})"


@dataclass(frozen=True, slots=True)
class _FulfilPlan:
    """Every read `ingest_loc`'s `_txn` needs to fulfil a `loc_req`,
    gathered during that transaction's read phase (`req_ref`/`req_snap`
    already showed a live, matching, non-expired request) so the write
    phase (`_fulfil_loc_req_in_txn` below) only ever stages writes --
    Firestore transactions require every read before any write (see
    `app/db/firestore.py`'s `run_transaction` docstring), and this module's
    dedup marker (`transaction.create(dedup_ref, ...)`) is itself a write
    that must come after all of these."""

    loc_req_msg_id: str
    loc_req_ref: DocumentReference
    loc_req_snap: DocumentSnapshot
    meta_ref: DocumentReference
    meta_snap: DocumentSnapshot
    requester_uids: list[str]
    conv_refs: dict[str, DocumentReference]
    conv_snaps: dict[str, DocumentSnapshot]
    # docs/FAMILIES_DESIGN.md §1 decisions 3-4: per-requester
    # `participants`/`familyIds` for the (owner, requester) pair, computed
    # during the read phase -- see the loop that builds this plan.
    pair_participants: dict[str, dict[str, dict[str, str]]]
    pair_family_ids: dict[str, list[str]]


def _fulfil_loc_req_in_txn(
    transaction: Transaction,
    *,
    device_id: str,
    owner_uid: str,
    env: LocEnvelope,
    loc_field: dict,
    req_ref: DocumentReference,
    plan: _FulfilPlan,
) -> None:
    """Write-only half of `loc_req` fulfilment (docs/SERVER_PLAN.md §5.6):
    (a) delete `locReqs/{deviceId}`, (b) mark the matching `loc_req`
    delivery `fulfilled`, (c) add one `kind='loc'` message to *each*
    coalesced requester's thread with the device owner -- "coalesced
    requests mean potentially multiple requesters share one `loc_req`...
    every one of them gets their own `kind='loc'` thread message"
    (§5.6). Called from inside `ingest_loc`'s own transaction (see that
    function's docstring for why it must not open its own), stages only
    writes:
    every read it needs was already done, into `plan`, during that
    transaction's read phase."""
    transaction.delete(req_ref)

    if plan.loc_req_snap.exists:
        loc_req_data = plan.loc_req_snap.to_dict() or {}
        loc_req_msg = Message.model_validate({"id": plan.loc_req_msg_id, **loc_req_data})
        bid = messages_store.find_pager_delivery(loc_req_msg, device_id)
        if bid is not None and loc_req_msg.deliveries[bid].state != "fulfilled":
            transaction.update(
                plan.loc_req_ref,
                {
                    f"deliveries.{bid}.state": "fulfilled",
                    "pendingDeviceIds": ArrayRemove([device_id]),
                },
            )

    seq = (plan.meta_snap.get("seqCounter") if plan.meta_snap.exists else 0) or 0
    for uid in plan.requester_uids:
        seq += 1
        key = messages_store.conv_key(owner_uid, uid)
        uids_sorted = sorted([owner_uid, uid])
        family_ids = plan.pair_family_ids[uid]
        new_msg_ref = messages_store.messages_ref(new_id("m_"))
        transaction.set(
            new_msg_ref,
            {
                "seq": seq,
                "convKey": key,
                "uids": uids_sorted,
                "senderUid": owner_uid,
                "recipientUid": uid,
                "kind": "loc",
                "body": None,
                "loc": loc_field,
                "wireId": None,
                "originBackendKind": "pager",
                "originBackendId": None,
                "ts": env.ts,
                "createdAt": SERVER_TIMESTAMP,
                "deliveries": {},
                "pendingDeviceIds": [],
                "familyIds": family_ids,
            },
        )
        conv_snap = plan.conv_snaps[uid]
        unread = dict(conv_snap.get("unread") or {}) if conv_snap.exists else {}
        unread[uid] = unread.get(uid, 0) + 1
        conv_data = {
            "uids": uids_sorted,
            "lastMessageAt": SERVER_TIMESTAMP,
            "lastPreview": _loc_preview(loc_field),
            "unread": unread,
        }
        if conv_snap.exists:
            transaction.update(plan.conv_refs[uid], conv_data)
        else:
            # docs/FAMILIES_DESIGN.md §1 decisions 3-4: a `loc` message can
            # be the very first thing between owner and requester, lazily
            # creating their conversation doc exactly like `create_message`
            # does -- same `participants`/`familyIds` write on creation.
            conv_data["participants"] = plan.pair_participants[uid]
            conv_data["familyIds"] = family_ids
            transaction.set(plan.conv_refs[uid], conv_data)

    if plan.meta_snap.exists:
        transaction.update(plan.meta_ref, {"seqCounter": seq})
    else:
        transaction.set(
            plan.meta_ref, {"schemaVersion": 2, "lastSweepAt": None, "seqCounter": seq}
        )


def effective_loc_req_state(msg: Message, delivery_state: str) -> str:
    """PROTOCOL.md §13.4: a `loc_req` delivery still `sent` `loc_req_ttl_s()`
    after the message's `createdAt` is `expired` -- derived at read time,
    never written anywhere. Scoped to `loc_req` only; the generic
    (non-`loc_req`) 24h derived expiry has no equivalent helper."""
    if msg.kind != "loc_req" or delivery_state != "sent" or msg.createdAt is None:
        return delivery_state
    if _age_seconds(msg.createdAt) >= loc_req_ttl_s():
        return "expired"
    return delivery_state


# ---------------------------------------------------------------------------
# /internal/tick addition -- SERVER_PLAN.md §5.8 item 2
# ---------------------------------------------------------------------------


def clear_stale_loc_reqs() -> int:
    """`/internal/tick`: delete every `locReqs/{deviceId}` doc older than
    `loc_req_ttl_s()`, so a fresh `/locate` doesn't coalesce onto a dead
    request -- `Location.locate`'s own freshness check would already refuse
    to coalesce onto one, this just keeps the collection from accumulating
    dead rows between calls. Not transactional: a concurrent `/locate`
    racing a stale doc's deletion is harmless either way, since `locate()`'s
    own transaction re-reads the doc fresh and makes the same freshness
    decision independently.

    Filters server-side (`where('createdAt', '<',
    cutoff)`) rather than streaming the whole collection and filtering in
    Python -- every `locReqs` doc always has `createdAt` set (it is written
    with `SERVER_TIMESTAMP` on every create/supersede in `Location.locate`,
    resolved before commit), so there is no "missing field" case to also
    catch here the way a defensive `is None` check would."""
    ttl = loc_req_ttl_s()
    cutoff = datetime.fromtimestamp(time.time() - ttl, tz=UTC)
    cleared = 0
    for snap in _loc_reqs().where(filter=FieldFilter("createdAt", "<", cutoff)).stream():
        snap.reference.delete()
        cleared += 1
    return cleared


# ---------------------------------------------------------------------------
# /locate -- PROTOCOL.md §13.3 rules 5-7; SERVER_PLAN.md §5.6 second bullet
# ---------------------------------------------------------------------------


class NoLocatableDevice(Exception):
    """Raised by `Location.locate` when the target device is revoked or has
    no enabled `pager` backend to deliver a fresh `loc_req` to --
    docs/SERVER_PLAN.md §5.6: "no pager -> 409 no locatable device". Only
    raised on the "claim a fresh slot" path: coalescing onto an existing
    request or answering from a cached fix never needs a working pager
    backend (someone already established one, or the answer needs no wire
    trip at all)."""


@dataclass(frozen=True, slots=True)
class LocateOutcome:
    request_id: str | None
    cached: bool
    fix: LocationFix | None = None


def _find_pager_backend(device: devices_store.Device) -> BackendRow | None:
    for b in backends_store.list_backends(device.ownerUid):
        if b.enabled and b.kind == "pager" and b.config.get("deviceId") == device.id:
            return b
    return None


def _join_after_contention(
    *,
    requester_uid: str,
    req_ref: DocumentReference,
    ttl: int,
    original_exc: ValueError,
) -> LocateOutcome:
    """Called only after `run_transaction`'s claim attempt exhausted its own
    retry budget with the client library's `ValueError`/`Aborted` pair (see
    `CONTENTION_REREAD_ATTEMPTS`'s docstring above). Re-reads `req_ref`
    (plain, non-transactional -- the claim transaction that just failed is
    proof enough that a transaction is not what is needed here) up to
    `CONTENTION_REREAD_ATTEMPTS` times, with AWS-style full-jitter
    exponential backoff between reads, and as soon as the winner's request is
    visible joins it exactly like the coalesce branch of `_txn` above
    (`ArrayUnion` -- idempotent and atomic without a transaction) and returns
    the same shape of outcome. Re-raises `original_exc` if no request ever
    shows up within the budget -- at that point every explanation left is a
    real failure, not a race, and swallowing it would silently drop the
    caller's `/locate` request."""
    for attempt in range(CONTENTION_REREAD_ATTEMPTS):
        snap = req_ref.get()
        if snap.exists:
            data = snap.to_dict() or {}
            if _age_seconds(data.get("createdAt")) < ttl:
                req_ref.update({"requesterUids": ArrayUnion([requester_uid])})
                return LocateOutcome(request_id=data.get("messageId"), cached=False)
        if attempt < CONTENTION_REREAD_ATTEMPTS - 1:
            cap = min(
                CONTENTION_REREAD_MAX_DELAY_S,
                CONTENTION_REREAD_BASE_DELAY_S * (2**attempt),
            )
            time.sleep(random.uniform(0, cap))
    raise original_exc


class Location:
    """Holds a `Routing` instance so `locate()`'s "claim a fresh slot"
    branch can deliver the new `loc_req` inline through
    `Routing.redeliver_pager` -- the same primitive the online-edge
    republish and `/internal/tick` retries use (see this module's
    docstring) -- rather than duplicating `BrokerClient.publish` plus the
    attempts/error/`pendingDeviceIds` bookkeeping here."""

    def __init__(self, routing: Routing) -> None:
        self._routing = routing

    def locate(
        self,
        *,
        requester_uid: str,
        device: devices_store.Device,
        ts: int | None = None,
        _retry_on_conflict: bool = True,
    ) -> LocateOutcome:
        """`_retry_on_conflict` is an internal recursion guard (S2's
        `transaction.create()`-vs-`AlreadyExists` retry below) -- callers
        never pass it."""
        if device.revokedAt is not None:
            raise NoLocatableDevice(device.id)
        ts = ts if ts is not None else int(time.time())
        req_ref = _loc_reqs().document(device.id)
        ttl = loc_req_ttl_s()
        # Hoisted out of `_txn` so this lookup goes
        # through the ordinary (non-transactional) store API rather than
        # `backends_store.list_backends`'s `.stream()` call running *inside*
        # `run_transaction`'s `fn`, which violates that helper's own
        # documented contract ("`fn` MUST only read with
        # `transaction.get(...)`" -- see app/db/firestore.py). Looked up
        # unconditionally (even on the coalesce/cached paths that never need
        # it) rather than threading a flag through, since it is one cheap
        # read and keeps `_txn` simple.
        pager_backend = _find_pager_backend(device)
        # docs/FAMILIES_DESIGN.md §1 decisions 3-4: `familyIds` for the
        # (requester, owner) pair, needed only by the "claim a fresh slot"
        # branch below. Hoisted out of `_txn` and read plainly (not
        # `transaction=transaction`) for the same reason as
        # `pager_backend` above, plus one more: users do not change during
        # a locate, so this `users/{uid}` read needs no transactional
        # consistency, and enrolling it in the transaction's read set only
        # widened the window for `locReqs/{device.id}` write contention
        # under N-way concurrent `/locate` calls (this module's own
        # concurrency test reproduced `run_transaction` exhausting its
        # 5-attempt retry budget once this read moved inside `_txn`).
        # Computed unconditionally, same "cheap and keeps `_txn` simple"
        # tradeoff as `pager_backend`.
        _, claim_family_ids = messages_store.build_participants_and_family_ids(
            [requester_uid, device.ownerUid]
        )

        def _txn(transaction: Transaction) -> tuple[str, str | None, LocationFix | None]:
            req_snap = req_ref.get(transaction=transaction)
            if req_snap.exists:
                req_data = req_snap.to_dict() or {}
                if _age_seconds(req_data.get("createdAt")) < ttl:
                    # §13.3 rule 5: coalesce -- append this requester (if not
                    # already in it) and hand back the *existing* request.
                    requesters = list(req_data.get("requesterUids") or [])
                    if requester_uid not in requesters:
                        transaction.update(
                            req_ref, {"requesterUids": ArrayUnion([requester_uid])}
                        )
                    return ("coalesced", req_data.get("messageId"), None)

            # No live request to coalesce onto (missing, or stale enough
            # that a fresh one supersedes it) -- §13.3 rule 6: a fix younger
            # than CACHED_ANSWER_WINDOW_S answers immediately, no wire trip.
            fix_query = (
                locations_store.locations_collection(device.id)
                .order_by("createdAt", direction="DESCENDING")
                .limit(1)
            )
            fix_snaps = fix_query.get(transaction=transaction)
            if fix_snaps:
                fix_snap = fix_snaps[0]
                fix_data = fix_snap.to_dict() or {}
                # docs/LOCATION_TRACKING_DESIGN.md §5 R2 (this task):
                # PROTOCOL.md §13.3 item 9's "the relay's item 6 never
                # answers from an unsolicited src:"cell" fix (req:null)" --
                # an unresolved-request cell report is a coarse periodic
                # point, not an answer to anyone's question, so it must not
                # suppress a fresh `loc_req` (which may get a real GNSS fix)
                # the way a *request-answering* fix (including one with
                # `src:"cell"` -- §13.3 rule 7 still requires that one to
                # satisfy rule 6, or a second requester within 60s of it
                # triggers a second `/down`) correctly does. Deliberately
                # does not look further back than this newest doc even when
                # it is disqualified this way (limit(1) above) -- an older
                # doc is, by definition, more stale, not less.
                unsolicited_cell = fix_data.get("src") == "cell" and fix_data.get("reqId") is None
                if not unsolicited_cell and _age_seconds(fix_data.get("createdAt")) < CACHED_ANSWER_WINDOW_S:
                    if req_snap.exists:
                        # Stale locReqs row superseded by this cached answer
                        # -- clean it up now rather than waiting for tick.
                        transaction.delete(req_ref)
                    fix = LocationFix.model_validate({"id": fix_snap.id, **fix_data})
                    return ("cached", None, fix)

            # Neither a live request nor a fresh fix -- claim the slot and
            # create a brand new loc_req, device-targeted (see module
            # docstring for why this isn't `Routing.send()`). `pager_backend`
            # was looked up before this transaction started (see above).
            if pager_backend is None:
                raise NoLocatableDevice(device.id)

            msg_id = new_id("m_")
            msg_ref = messages_store.messages_ref(msg_id)
            conv_key = messages_store.conv_key(requester_uid, device.ownerUid)
            meta_ref = messages_store.meta_ref()
            meta_snap = meta_ref.get(transaction=transaction)
            # docs/FAMILIES_DESIGN.md §1 decisions 3-4: `familyIds` on every
            # message write -- `claim_family_ids` was computed before this
            # transaction started (see above).
            family_ids = claim_family_ids

            seq = ((meta_snap.get("seqCounter") if meta_snap.exists else 0) or 0) + 1
            if meta_snap.exists:
                transaction.update(meta_ref, {"seqCounter": seq})
            else:
                transaction.set(
                    meta_ref, {"schemaVersion": 2, "lastSweepAt": None, "seqCounter": seq}
                )

            uids_sorted = sorted([requester_uid, device.ownerUid])
            transaction.set(
                msg_ref,
                {
                    "seq": seq,
                    "convKey": conv_key,
                    "uids": uids_sorted,
                    "senderUid": requester_uid,
                    "recipientUid": device.ownerUid,
                    "kind": "loc_req",
                    "body": None,
                    "loc": None,
                    "wireId": None,
                    "originBackendKind": "webapp",
                    "originBackendId": None,
                    "ts": ts,
                    "createdAt": SERVER_TIMESTAMP,
                    "deliveries": {
                        pager_backend.id: {
                            "kind": "pager",
                            "state": "queued",
                            "attempts": 0,
                            "externalId": device.id,
                        }
                    },
                    "pendingDeviceIds": [device.id],
                    "familyIds": family_ids,
                },
            )
            # PROTOCOL.md §3.2: a loc_req "is not a thread entry" -- unlike
            # an ordinary send, `conversations/{convKey}` (lastPreview/
            # unread) is deliberately left untouched here; only the
            # eventual `kind='loc'` answer (`_fulfil_loc_req_in_txn` above)
            # touches the conversation summary.

            req_payload = {
                "messageId": msg_id,
                "requesterUids": [requester_uid],
                "createdAt": SERVER_TIMESTAMP,
            }
            if req_snap.exists:
                # Superseding a stale/expired request (already established
                # above -- either past `ttl`, or with no fresh fix to
                # answer from) -- an ordinary overwrite, not a new
                # uniqueness claim, so `set()` is correct here.
                transaction.set(req_ref, req_payload)
            else:
                # PROTOCOL.md §13.3 rule 5, stated normatively -- "at
                # most one in-flight loc_req per device" -- is an explicit
                # precondition of this
                # write, the same `transaction.create()`-or-`AlreadyExists`
                # idiom every other uniqueness invariant in this codebase
                # uses (`wireIds`, `aliases`, this module's own
                # `locWireIds`), rather than relying on Firestore's
                # read-lock semantics (a concurrent second claimer aborts
                # and retries, then coalesces on retry) to make the
                # invariant hold only by accident of transaction isolation.
                # `AlreadyExists` here means a concurrent claimer's
                # transaction committed first between our read and our
                # write; caught outside the transaction below and retried
                # once, so the retry's read sees the now-existing doc and
                # correctly coalesces instead of claiming again.
                transaction.create(req_ref, req_payload)
            return ("claimed", msg_id, None)

        try:
            kind, request_id, fix = run_transaction(_txn)
        except AlreadyExists:
            if not _retry_on_conflict:
                raise
            return self.locate(
                requester_uid=requester_uid, device=device, ts=ts, _retry_on_conflict=False
            )
        except ValueError as exc:
            # `run_transaction`'s own retry-exhaustion shape (see
            # CONTENTION_REREAD_ATTEMPTS's docstring above) -- anything else
            # (a genuine bug raised by `_txn`, e.g. `NoLocatableDevice` is
            # its own type and never hits this clause) propagates unchanged.
            if not isinstance(exc.__cause__, Aborted):
                raise
            return _join_after_contention(
                requester_uid=requester_uid, req_ref=req_ref, ttl=ttl, original_exc=exc
            )

        if kind == "claimed":
            assert request_id is not None
            msg = messages_store.get_message(request_id)
            assert msg is not None
            self._routing.redeliver_pager(msg, device.id)
            # PROTOCOL.md §13.3 rule 5's coalescing is for an *in-flight*
            # request, not a dead one. If that inline
            # delivery attempt just exhausted `record_delivery_attempt`'s
            # 5-attempt cap and reached 'failed' (e.g. a permanently broken
            # pager backend), leaving `locReqs/{device.id}` in place would
            # make every subsequent `/locate` call coalesce onto a request
            # that can never be answered. Re-read the delivery state and,
            # if it is now 'failed', clear the row immediately and surface
            # the same 409 "no locatable device" outcome the no-pager-
            # backend case uses, rather than telling the caller a request
            # is pending when it can never resolve.
            refreshed = messages_store.get_message(request_id)
            assert refreshed is not None
            bid = messages_store.find_pager_delivery(refreshed, device.id)
            if bid is not None and refreshed.deliveries[bid].state == "failed":
                req_ref.delete()
                raise NoLocatableDevice(device.id)
            return LocateOutcome(request_id=request_id, cached=False)
        if kind == "coalesced":
            return LocateOutcome(request_id=request_id, cached=False)
        return LocateOutcome(request_id=None, cached=True, fix=fix)

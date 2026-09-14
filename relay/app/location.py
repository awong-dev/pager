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
   and PROTOCOL.md §13.2's "in the same transaction" taken literally: dedup
   and fulfilment used to be two separate transactions here, which left a
   window where a crash/timeout between them made a webhook redelivery see
   the dedup marker already committed and silently swallow the retry,
   leaving the `loc_req` stuck `sent` forever with no requester ever
   notified -- `build finding, S1`). A periodic fix (`req: null`) is never a
   thread message, per §3.2/§5.6 -- only the `locations` collection entry.

2. **`/locate`** (`Location.locate`, called by
   `app/routers/conversations.py`'s `POST /api/conversations/{alias}/locate`):
   one transaction on `locReqs/{deviceId}` implementing PROTOCOL.md §13.3
   rules 5-7 -- coalesce onto a live (<15 min) request, answer from a
   <60s-old cached fix with no wire traffic at all, or claim the slot and
   create a fresh `loc_req` message, delivered inline through the same
   `Routing.redeliver_pager` primitive the online-edge republish and
   `/internal/tick` retries use.

**Schema footnote** (flagged per this phase's brief, same convention as
`app/routing.py`'s `originBackendKind`/`originBackendId` note): §3's table
does not list a dedup collection for `/loc` envelopes the way it lists
`wireIds/{wireId}_{recipientUid}` for up messages. `locWireIds/{id}` below
is the same *pattern* (a `transaction.create()`-or-`AlreadyExists` dedup
marker, one field: `deviceId`) applied to `/loc`'s own id space (`l_` + 8
hex, §1) instead of reusing `wireIds`, kept as a separate collection because
the two dedup keys are shaped differently (`wireIds` docs are keyed
`{wireId}_{recipientUid}`; a `/loc` fix has no recipient to key against).
This is additive to §3, not a change to it.

**`loc_req` is device-targeted, not user-targeted** (per this phase's
brief, and the reason `Location.locate` does not go through
`app.routing.Routing.send()`): unlike an ordinary text send, which fans a
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
pager delivery's, per this phase's brief ("the delivery must still go
through `BrokerClient.publish` and update delivery/message state the same
way other pager deliveries do").
"""

from __future__ import annotations

import logging
import os
import time
from dataclasses import dataclass
from datetime import UTC, datetime

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import (
    SERVER_TIMESTAMP,
    ArrayRemove,
    ArrayUnion,
    DocumentReference,
    DocumentSnapshot,
    FieldFilter,
    Transaction,
)

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
from app.wire import LocEnvelope

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


def _fix_doc(env: LocEnvelope) -> dict:
    assert env.loc is not None
    return {
        "ts": env.ts,
        "fixTs": env.loc.fix_ts,
        "lat": env.loc.lat,
        "lon": env.loc.lon,
        "accM": env.loc.acc,
        "src": env.loc.src,
        "cached": env.cached,
        "reqId": env.req,
        "createdAt": SERVER_TIMESTAMP,
    }


def ingest_loc(device_id: str, env: LocEnvelope) -> None:
    """§13.2: dedup on `id`, storing the fix (if any -- `env.loc` is `None`
    when the device answers `err:"no_fix"`/`"disabled"`, in which case there
    is nothing to add to `locations`, only a `loc_req` to resolve, if `req`
    is set) and resolving the matching `loc_req` (`_fulfil_loc_req_in_txn`
    below), **all in one transaction** -- `build finding, S1`: dedup and
    fulfilment used to be two separate transactions (a `run_transaction`
    call here, then a second one in a since-removed `_fulfil_loc_req`
    helper). If the second one ever failed after the first had already
    committed -- a Firestore abort, a Cloud Run timeout, a crash -- the
    webhook returned a 500, EMQX redelivered the QoS-1 `/loc` message, and
    the dedup check on retry saw the id already recorded and silently
    swallowed the redelivery: the fix got stored, but the `loc_req`
    delivery stayed stuck `sent` forever (until the 15-minute derived
    expiry) and no requester was ever notified. Folding fulfilment into the
    same transaction as the dedup marker means both commit or neither does,
    so a webhook redelivery after a partial failure retries the whole thing
    and reaches the same end state a first-try success would have. A
    periodic fix (`req: None`) that dedups clean simply updates
    `locations`; nothing else happens."""
    dedup_ref = _loc_wire_ids().document(env.id)
    fix_ref = locations_store.new_location_ref(device_id) if env.loc is not None else None
    fix_data = _fix_doc(env) if env.loc is not None else None

    # The device lookup (unlike everything `_fulfil_loc_req_in_txn` reads)
    # is not folded into the transaction: it is not part of any invariant
    # this function needs atomicity for (an unknown/renamed device is a
    # logged-and-dropped case either way, same as `_find_pager_backend`'s
    # non-transactional read in `Location.locate`), and reading it inside
    # the transaction would gain nothing but an extra round trip.
    owner_uid: str | None = None
    if env.req is not None:
        device = devices_store.get_device(device_id)
        if device is None:
            logger.warning("/loc req=%s for unknown device=%s dropped", env.req, device_id)
        else:
            owner_uid = device.ownerUid

    req_ref = _loc_reqs().document(device_id) if owner_uid is not None else None
    loc_field = _loc_message_field(env) if owner_uid is not None else None
    ttl = loc_req_ttl_s()

    def _txn(transaction: Transaction) -> None:
        # -- every read this transaction needs, before any write is staged
        # (Firestore's own local rule, see app/db/firestore.py's
        # run_transaction docstring) --
        fulfil: _FulfilPlan | None = None
        drop_reason: str | None = None
        delete_stale_req = False

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
                    requester_uids = list(req_data.get("requesterUids") or [])
                    loc_req_ref = messages_store.messages_ref(loc_req_msg_id)
                    loc_req_snap = loc_req_ref.get(transaction=transaction)
                    meta_ref = messages_store.meta_ref()
                    meta_snap = meta_ref.get(transaction=transaction)
                    conv_refs: dict[str, DocumentReference] = {}
                    conv_snaps: dict[str, DocumentSnapshot] = {}
                    for uid in requester_uids:
                        key = messages_store.conv_key(owner_uid, uid)
                        ref = messages_store.conversation_ref(key)
                        conv_refs[uid] = ref
                        conv_snaps[uid] = ref.get(transaction=transaction)
                    fulfil = _FulfilPlan(
                        loc_req_msg_id=loc_req_msg_id,
                        loc_req_ref=loc_req_ref,
                        loc_req_snap=loc_req_snap,
                        meta_ref=meta_ref,
                        meta_snap=meta_snap,
                        requester_uids=requester_uids,
                        conv_refs=conv_refs,
                        conv_snaps=conv_snaps,
                    )

        # -- every write from here on --

        # `transaction.create` raises AlreadyExists (caught below, outside
        # the transaction) if this `/loc` id was already processed -- same
        # idiom `app/store/messages.py`'s `create_message` uses for its
        # `wireIds` dedup.
        transaction.create(dedup_ref, {"deviceId": device_id, "createdAt": SERVER_TIMESTAMP})
        if fix_ref is not None:
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


def _loc_message_field(env: LocEnvelope) -> dict:
    """The `loc` field of the `kind='loc'` thread message posted to each
    requester -- a real fix's shape, or `{"err": ...}` when the device
    answered `no_fix`/`disabled` (still worth telling the requester, rather
    than leaving them to find out only from the 15-minute `expired` derived
    state)."""
    if env.loc is not None:
        return {
            "lat": env.loc.lat,
            "lon": env.loc.lon,
            "accM": env.loc.acc,
            "fixTs": env.loc.fix_ts,
            "src": env.loc.src,
            "cached": env.cached,
        }
    return {"err": env.err}


def _loc_preview(env: LocEnvelope) -> str:
    return "location" if env.loc is not None else f"location unavailable ({env.err})"


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
    every one of them gets their own `kind='loc'` thread message" (this
    phase's brief). Called from inside `ingest_loc`'s own transaction
    (`build finding, S1` -- see `ingest_loc`'s docstring for why this is no
    longer its own separate `run_transaction` call), stages only writes:
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
            },
        )
        conv_snap = plan.conv_snaps[uid]
        unread = dict(conv_snap.get("unread") or {}) if conv_snap.exists else {}
        unread[uid] = unread.get(uid, 0) + 1
        conv_data = {
            "uids": uids_sorted,
            "lastMessageAt": SERVER_TIMESTAMP,
            "lastPreview": _loc_preview(env),
            "unread": unread,
        }
        if conv_snap.exists:
            transaction.update(plan.conv_refs[uid], conv_data)
        else:
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
    never written anywhere. Mirrors `app/store/legacy.py`'s `LegacyMessage.
    effective_state` for the MVP model's 24h rule; the v2 model's generic
    (non-`loc_req`) 24h derived-expiry has no equivalent helper yet (out of
    this phase's scope) -- this one is deliberately scoped to `loc_req`
    only, per this phase's brief."""
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

    `build finding` (nit): filters server-side (`where('createdAt', '<',
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
        # `build finding` (nit): hoisted out of `_txn` so this lookup goes
        # through the ordinary (non-transactional) store API rather than
        # `backends_store.list_backends`'s `.stream()` call running *inside*
        # `run_transaction`'s `fn`, which violates that helper's own
        # documented contract ("`fn` MUST only read with
        # `transaction.get(...)`" -- see app/db/firestore.py). Looked up
        # unconditionally (even on the coalesce/cached paths that never need
        # it) rather than threading a flag through, since it is one cheap
        # read and keeps `_txn` simple.
        pager_backend = _find_pager_backend(device)

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
                if _age_seconds(fix_data.get("createdAt")) < CACHED_ANSWER_WINDOW_S:
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
                # `build finding, S2`: PROTOCOL.md §13.3 rule 5, stated
                # normatively -- "at most one in-flight loc_req per
                # device" -- make that an explicit precondition of this
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

        if kind == "claimed":
            assert request_id is not None
            msg = messages_store.get_message(request_id)
            assert msg is not None
            self._routing.redeliver_pager(msg, device.id)
            # `build finding, S3`: PROTOCOL.md §13.3 rule 5's coalescing is
            # for an *in-flight* request, not a dead one. If that inline
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

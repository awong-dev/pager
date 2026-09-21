"""Direct unit tests for `app.location` -- docs/PROTOCOL.md §13,
docs/SERVER_PLAN.md §5.6: `/loc` ingest (dedup, periodic vs. on-demand,
`loc_req` fulfilment/coalescing), `/locate`'s coalescing transaction, the
<60s cached-answer path, the `no_fix` path, and derived expiry. Router-level
coverage (`POST /api/conversations/{alias}/locate`'s HTTP status mapping) is
`tests/test_conversations.py`; this file exercises `app.location`'s
functions directly against the emulator, the same pattern
`tests/test_routing.py`/`tests/test_jobs.py` use for their modules.
"""

from __future__ import annotations

import os
import threading
import time
from datetime import UTC, datetime, timedelta

import pytest
from google.cloud.firestore import FieldFilter

from app import cellgeo, location
from app.ids import new_id
from app.routing import Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import locations as locations_store
from app.store import messages as messages_store
from app.store import users as users_store
from app.store.locations import LocationFix
from app.wire import CellInfo, LocEnvelope, LocFix
from tests.fake_transport import FakeBrokerClient


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


def _make_pager_device(device_id: str, owner_uid: str):
    # S1.4: `auth_mode="password"` -- this module's tests are about
    # /locate's claim/dedup/delivery logic, not device signing.
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )
    return backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


def _loc_env(
    *,
    req: str | None = None,
    lat: float = 37.7749,
    lon: float = -122.4194,
    err: str | None = None,
    cell: CellInfo | None = None,
) -> LocEnvelope:
    ts = int(time.time())
    loc = None if err else LocFix(lat=lat, lon=lon, fix_ts=ts, src="gnss")
    return LocEnvelope(id=new_id("l_"), ts=ts, loc=loc, req=req, cached=False, err=err, cell=cell)


def _cell_info(**overrides) -> CellInfo:
    obj = {"mcc": "310", "mnc": "410", "tac": 12345, "ci": 87654321, "rsrp": -95}
    obj.update(overrides)
    return CellInfo.model_validate(obj)


def _setup() -> tuple[Routing, FakeBrokerClient, location.Location]:
    broker = FakeBrokerClient()
    routing = Routing(broker)
    return routing, broker, location.Location(routing)


def _backdate_loc_req(device_id: str, seconds_ago: int) -> None:
    """Directly overwrites `locReqs/{device_id}.createdAt` to simulate a
    request that's aged past its TTL, without sleeping in a test."""
    from app.db.firestore import get_db

    ref = get_db().collection("locReqs").document(device_id)
    data = ref.get().to_dict()
    data["createdAt"] = datetime.now(UTC) - timedelta(seconds=seconds_ago)
    ref.set(data)


@pytest.fixture(autouse=True)
def _default_ttl(monkeypatch: pytest.MonkeyPatch) -> None:
    # Keep the default (900s) unless a test overrides it explicitly --
    # asserting the env var round-trips correctly.
    monkeypatch.delenv("LOC_REQ_TTL_S", raising=False)


# ---------------------------------------------------------------------------
# /loc ingest: periodic fixes
# ---------------------------------------------------------------------------


def test_periodic_fix_is_stored_and_is_not_a_thread_message():
    _make_user("student", "student")
    _make_pager_device("pgr-loc-1", "student")

    env = _loc_env()
    location.ingest_loc("pgr-loc-1", env)

    fixes = locations_store.list_locations("pgr-loc-1")
    assert len(fixes) == 1
    assert fixes[0].lat == 37.7749
    assert fixes[0].reqId is None

    # Periodic fixes never touch messages/conversations.
    assert messages_store.get_conversation(messages_store.conv_key("student", "student")) is None


def test_duplicate_loc_id_is_deduped_not_stored_twice():
    _make_user("student", "student")
    _make_pager_device("pgr-loc-2", "student")

    env = _loc_env()
    location.ingest_loc("pgr-loc-2", env)
    location.ingest_loc("pgr-loc-2", env)  # same id, e.g. a webhook retry

    assert len(locations_store.list_locations("pgr-loc-2")) == 1


# ---------------------------------------------------------------------------
# /locate: fresh claim, no locatable device
# ---------------------------------------------------------------------------


def test_locate_with_no_pager_backend_raises_no_locatable_device():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    device = devices_store.create_device(
        device_id="pgr-loc-3",
        owner_uid="student",
        label="d",
        mqtt_username="pgr-loc-3",
        mqtt_password_hash="x",
    )
    # Deliberately no pager backend row for this device.

    _routing, _broker, loc = _setup()
    with pytest.raises(location.NoLocatableDevice):
        loc.locate(requester_uid="mom", device=device)


def test_locate_creates_loc_req_and_delivers_through_pager_backend():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-4", "student")
    device = devices_store.get_device("pgr-loc-4")

    _routing, broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)

    assert outcome.cached is False
    assert outcome.request_id is not None
    assert len(broker.published) == 1
    sent = messages_store.get_message(outcome.request_id)
    assert sent is not None
    assert sent.kind == "loc_req"
    assert sent.senderUid == "mom"
    assert sent.recipientUid == "student"
    pager_bid = next(bid for bid, d in sent.deliveries.items() if d.kind == "pager")
    assert sent.deliveries[pager_bid].state == "sent"

    # loc_req is not a thread entry -- no conversation summary written for it.
    conv = messages_store.get_conversation(messages_store.conv_key("mom", "student"))
    assert conv is None


# ---------------------------------------------------------------------------
# /locate: coalescing
# ---------------------------------------------------------------------------


def test_second_requester_coalesces_onto_the_same_loc_req():
    _make_user("mom", "mom")
    _make_user("dad", "dad")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    allow_store.set_edge("dad", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-5", "student")
    device = devices_store.get_device("pgr-loc-5")

    _routing, broker, loc = _setup()
    first = loc.locate(requester_uid="mom", device=device)
    second = loc.locate(requester_uid="dad", device=device)

    assert first.request_id == second.request_id
    assert len(broker.published) == 1  # only the first claim published anything

    from app.db.firestore import get_db

    req_doc = get_db().collection("locReqs").document("pgr-loc-5").get().to_dict()
    assert set(req_doc["requesterUids"]) == {"mom", "dad"}


def test_locate_twice_by_the_same_requester_is_idempotent_in_requester_list():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-5b", "student")
    device = devices_store.get_device("pgr-loc-5b")

    _routing, _broker, loc = _setup()
    first = loc.locate(requester_uid="mom", device=device)
    second = loc.locate(requester_uid="mom", device=device)
    assert first.request_id == second.request_id

    from app.db.firestore import get_db

    req_doc = get_db().collection("locReqs").document("pgr-loc-5b").get().to_dict()
    assert req_doc["requesterUids"] == ["mom"]


def test_locate_does_not_coalesce_onto_a_stale_loc_req():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-6", "student")
    device = devices_store.get_device("pgr-loc-6")

    _routing, broker, loc = _setup()
    first = loc.locate(requester_uid="mom", device=device)
    _backdate_loc_req("pgr-loc-6", seconds_ago=location.DEFAULT_LOC_REQ_TTL_S + 1)

    second = loc.locate(requester_uid="mom", device=device)
    assert second.request_id != first.request_id
    assert len(broker.published) == 2  # each claim delivers its own loc_req


# ---------------------------------------------------------------------------
# /locate: <60s cached answer
# ---------------------------------------------------------------------------


def test_locate_answers_from_a_fresh_cached_fix_with_no_wire_traffic():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-7", "student")
    device = devices_store.get_device("pgr-loc-7")

    locations_store.add_location(
        "pgr-loc-7", LocationFix(ts=int(time.time()), fixTs=int(time.time()), lat=1.0, lon=2.0)
    )

    _routing, broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)

    assert outcome.cached is True
    assert outcome.request_id is None
    assert outcome.fix is not None
    assert outcome.fix.lat == 1.0
    assert broker.published == []

    from app.db.firestore import get_db

    assert not get_db().collection("locReqs").document("pgr-loc-7").get().exists


def test_locate_cached_answer_deletes_a_stale_loc_req_row():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-7b", "student")
    device = devices_store.get_device("pgr-loc-7b")

    _routing, _broker, loc = _setup()
    loc.locate(requester_uid="mom", device=device)
    _backdate_loc_req("pgr-loc-7b", seconds_ago=location.DEFAULT_LOC_REQ_TTL_S + 1)
    locations_store.add_location(
        "pgr-loc-7b", LocationFix(ts=int(time.time()), fixTs=int(time.time()), lat=3.0, lon=4.0)
    )

    outcome = loc.locate(requester_uid="mom", device=device)
    assert outcome.cached is True

    from app.db.firestore import get_db

    assert not get_db().collection("locReqs").document("pgr-loc-7b").get().exists


# ---------------------------------------------------------------------------
# /loc ingest: loc_req fulfilment
# ---------------------------------------------------------------------------


def test_loc_answer_fulfils_the_request_and_posts_a_loc_thread_message():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-8", "student")
    device = devices_store.get_device("pgr-loc-8")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    req_id = outcome.request_id
    assert req_id is not None

    answer = _loc_env(req=req_id, lat=9.0, lon=8.0)
    location.ingest_loc("pgr-loc-8", answer)

    loc_req_msg = messages_store.get_message(req_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "fulfilled"
    assert "pgr-loc-8" not in loc_req_msg.pendingDeviceIds

    from app.db.firestore import get_db

    assert not get_db().collection("locReqs").document("pgr-loc-8").get().exists

    thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    loc_msgs = [m for m in thread if m.kind == "loc"]
    assert len(loc_msgs) == 1
    assert loc_msgs[0].loc["lat"] == 9.0
    assert loc_msgs[0].senderUid == "student"
    assert loc_msgs[0].recipientUid == "mom"


def test_loc_answer_fulfils_for_every_coalesced_requester():
    _make_user("mom", "mom")
    _make_user("dad", "dad")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    allow_store.set_edge("dad", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-9", "student")
    device = devices_store.get_device("pgr-loc-9")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    loc.locate(requester_uid="dad", device=device)
    req_id = outcome.request_id

    location.ingest_loc("pgr-loc-9", _loc_env(req=req_id))

    mom_thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    dad_thread = messages_store.list_thread(messages_store.conv_key("dad", "student"))
    assert len([m for m in mom_thread if m.kind == "loc"]) == 1
    assert len([m for m in dad_thread if m.kind == "loc"]) == 1


def test_no_fix_answer_still_fulfils_and_notes_the_error_in_the_thread():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-10", "student")
    device = devices_store.get_device("pgr-loc-10")

    _routing, broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    broker.clear()

    location.ingest_loc("pgr-loc-10", _loc_env(req=outcome.request_id, err="no_fix"))

    loc_req_msg = messages_store.get_message(outcome.request_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "fulfilled"

    thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    loc_msgs = [m for m in thread if m.kind == "loc"]
    assert len(loc_msgs) == 1
    assert loc_msgs[0].loc == {"err": "no_fix"}

    # No fix was obtained -> nothing new in `locations`.
    assert locations_store.list_locations("pgr-loc-10") == []


def test_late_loc_answer_for_an_already_fulfilled_request_is_dropped():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-11", "student")
    device = devices_store.get_device("pgr-loc-11")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    location.ingest_loc("pgr-loc-11", _loc_env(req=outcome.request_id))

    thread_before = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    # A second, late answer for the same (now resolved) request.
    location.ingest_loc("pgr-loc-11", _loc_env(req=outcome.request_id))
    thread_after = messages_store.list_thread(messages_store.conv_key("mom", "student"))

    assert len(thread_after) == len(thread_before)  # no extra 'loc' message posted


def test_late_loc_answer_past_ttl_is_dropped_not_fulfilled():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-12", "student")
    device = devices_store.get_device("pgr-loc-12")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    _backdate_loc_req("pgr-loc-12", seconds_ago=location.DEFAULT_LOC_REQ_TTL_S + 1)

    location.ingest_loc("pgr-loc-12", _loc_env(req=outcome.request_id))

    loc_req_msg = messages_store.get_message(outcome.request_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "sent"  # never marked fulfilled

    thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    assert [m for m in thread if m.kind == "loc"] == []


# ---------------------------------------------------------------------------
# derived expiry
# ---------------------------------------------------------------------------


def test_effective_loc_req_state_derives_expired_past_ttl():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-13", "student")
    device = devices_store.get_device("pgr-loc-13")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    msg = messages_store.get_message(outcome.request_id)
    pager_bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "pager")

    assert location.effective_loc_req_state(msg, msg.deliveries[pager_bid].state) == "sent"

    # Backdate the message itself (not locReqs) to simulate the passage of
    # time for the derived-state check, which reads msg.createdAt directly.
    from app.db.firestore import get_db

    ref = get_db().collection("messages").document(outcome.request_id)
    ref.update({"createdAt": datetime.now(UTC) - timedelta(seconds=location.DEFAULT_LOC_REQ_TTL_S + 1)})
    msg2 = messages_store.get_message(outcome.request_id)
    assert (
        location.effective_loc_req_state(msg2, msg2.deliveries[pager_bid].state) == "expired"
    )


def test_effective_loc_req_state_leaves_non_sent_states_alone():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-14", "student")
    device = devices_store.get_device("pgr-loc-14")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    location.ingest_loc("pgr-loc-14", _loc_env(req=outcome.request_id))
    msg = messages_store.get_message(outcome.request_id)
    pager_bid = next(bid for bid, d in msg.deliveries.items() if d.kind == "pager")
    assert msg.deliveries[pager_bid].state == "fulfilled"
    assert location.effective_loc_req_state(msg, "fulfilled") == "fulfilled"


def test_loc_answer_retry_after_partial_fulfilment_failure_is_not_lost(
    monkeypatch: pytest.MonkeyPatch,
):
    """Dedup and fulfilment must not be two
    separate transactions -- a crash partway through the second one left
    the dedup marker committed (from the first) but the `loc_req` never
    resolved, so a webhook redelivery of the *same* `/loc` envelope saw the
    id already deduped and silently swallowed the retry, leaving the
    request stuck `sent` forever with no requester ever notified. Now
    they're one transaction: force the fulfilment half to raise partway
    through on the first call (simulating the crash/timeout/abort between
    the old two transactions), confirm the *whole* transaction -- dedup
    marker included -- rolled back with it, then re-ingest the exact same
    envelope (the redelivery an EMQX QoS-1 retry produces) and confirm the
    retry reaches fulfilled with no silent-loss window."""
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-18", "student")
    device = devices_store.get_device("pgr-loc-18")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)
    req_id = outcome.request_id
    assert req_id is not None

    answer = _loc_env(req=req_id, lat=5.0, lon=6.0)

    calls = {"n": 0}
    original = location.messages_store.find_pager_delivery

    def _raise_once_then_delegate(*args, **kwargs):
        calls["n"] += 1
        if calls["n"] == 1:
            raise RuntimeError("simulated crash mid-fulfilment")
        return original(*args, **kwargs)

    monkeypatch.setattr(location.messages_store, "find_pager_delivery", _raise_once_then_delegate)

    # First attempt (the webhook's original delivery of this /loc
    # envelope): the fulfilment half of ingest_loc's single transaction
    # raises before it can return, so nothing in this transaction commits --
    # not the dedup marker, not the fulfilment writes.
    with pytest.raises(RuntimeError):
        location.ingest_loc("pgr-loc-18", answer)

    from app.db.firestore import get_db

    # The crux of S1's fix: before it, this dedup marker would already be
    # committed (from the first, now-removed, separate transaction) even
    # though fulfilment never happened -- and the assertion below would
    # fail.
    assert not get_db().collection("locWireIds").document(answer.id).get().exists
    loc_req_msg = messages_store.get_message(req_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "sent"
    thread_before_retry = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    assert [m for m in thread_before_retry if m.kind == "loc"] == []

    # Second attempt: the *exact same* envelope -- exactly what EMQX
    # redelivers when the relay's HTTP response to the first attempt never
    # made it back (crash, timeout, Firestore abort).
    location.ingest_loc("pgr-loc-18", answer)

    loc_req_msg = messages_store.get_message(req_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "fulfilled"
    assert not get_db().collection("locReqs").document("pgr-loc-18").get().exists

    thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    loc_msgs = [m for m in thread if m.kind == "loc"]
    assert len(loc_msgs) == 1
    assert loc_msgs[0].loc["lat"] == 5.0


# ---------------------------------------------------------------------------
# /locate: concurrent claims (S2) and a permanently-failed delivery (S3)
# ---------------------------------------------------------------------------


def test_locate_concurrent_claims_produce_exactly_one_loc_req():
    """The "no existing request" claim path
    uses `transaction.create()` (AlreadyExists on conflict, retried once)
    instead of `set()`, making PROTOCOL.md §13.3 rule 5's "at most one
    in-flight loc_req per device" an explicit precondition rather than an
    accident of Firestore's read-lock semantics. N threads racing
    `Location.locate` on the same device with no live request must still
    produce exactly one `loc_req` message and exactly one broker publish,
    regardless of thread count."""
    _make_user("student", "student")
    n = 8
    requesters = [f"requester-{i}" for i in range(n)]
    for uid in requesters:
        _make_user(uid, uid)
        allow_store.set_edge(uid, "student", message=True, locate=True)
    _make_pager_device("pgr-loc-19", "student")
    device = devices_store.get_device("pgr-loc-19")

    _routing, broker, loc = _setup()

    results: list[location.LocateOutcome] = []
    errors: list[BaseException] = []
    barrier = threading.Barrier(n)

    def worker(uid: str) -> None:
        barrier.wait()
        try:
            results.append(loc.locate(requester_uid=uid, device=device))
        except BaseException as exc:  # noqa: BLE001 -- captured for the assertion below
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(uid,)) for uid in requesters]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert errors == []
    assert len(results) == n
    request_ids = {r.request_id for r in results}
    assert len(request_ids) == 1  # every thread coalesced onto the same claim
    assert len(broker.published) == 1  # only the winning claim delivered anything

    from app.db.firestore import get_db

    req_doc = get_db().collection("locReqs").document("pgr-loc-19").get().to_dict()
    assert set(req_doc["requesterUids"]) == set(requesters)

    loc_req_msg_ids = {
        snap.id
        for snap in get_db()
        .collection("messages")
        .where(filter=FieldFilter("recipientUid", "==", "student"))
        .where(filter=FieldFilter("kind", "==", "loc_req"))
        .stream()
    }
    assert len(loc_req_msg_ids) == 1


def test_locate_permanently_failed_delivery_clears_the_dead_loc_req(
    monkeypatch: pytest.MonkeyPatch,
):
    """PROTOCOL.md §13.3 rule 5's coalescing is
    for an *in-flight* request, not a dead one. If the inline pager
    delivery a fresh claim triggers exhausts `record_delivery_attempt`'s
    attempts cap and reaches 'failed', `locate()` must clear
    `locReqs/{deviceId}` immediately rather than leaving every subsequent
    `/locate` call coalescing onto a request that can never be answered.
    Caps `record_delivery_attempt`'s `max_attempts` at 1 (rather than the
    default 5) so a single always-failing publish reaches 'failed' within
    one `locate()` call, exactly the case this test needs to exercise."""
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-20", "student")
    device = devices_store.get_device("pgr-loc-20")

    broker = FakeBrokerClient()
    broker.fail_publish = True
    routing = Routing(broker)
    loc = location.Location(routing)

    original_record = location.messages_store.record_delivery_attempt

    def _record_capped_at_one_attempt(*args, **kwargs):
        kwargs.setdefault("max_attempts", 1)
        return original_record(*args, **kwargs)

    monkeypatch.setattr(
        location.messages_store, "record_delivery_attempt", _record_capped_at_one_attempt
    )

    from app.db.firestore import get_db

    def _loc_req_ids() -> set[str]:
        return {
            snap.id
            for snap in get_db()
            .collection("messages")
            .where(filter=FieldFilter("recipientUid", "==", "student"))
            .where(filter=FieldFilter("kind", "==", "loc_req"))
            .stream()
        }

    with pytest.raises(location.NoLocatableDevice):
        loc.locate(requester_uid="mom", device=device)

    assert not get_db().collection("locReqs").document("pgr-loc-20").get().exists
    first_ids = _loc_req_ids()
    assert len(first_ids) == 1

    # A second call must claim a *fresh* slot, not coalesce onto the dead
    # one -- it also fails (the broker is still configured to always fail)
    # and also surfaces the same 409-equivalent, but it must create its own
    # new loc_req rather than silently reusing the first (unanswerable) one.
    with pytest.raises(location.NoLocatableDevice):
        loc.locate(requester_uid="mom", device=device)

    assert not get_db().collection("locReqs").document("pgr-loc-20").get().exists
    second_ids = _loc_req_ids()
    assert len(second_ids) == 2
    assert first_ids < second_ids


def test_loc_req_ttl_s_env_override():
    os.environ["LOC_REQ_TTL_S"] = "5"
    try:
        assert location.loc_req_ttl_s() == 5
    finally:
        del os.environ["LOC_REQ_TTL_S"]
    assert location.loc_req_ttl_s() == location.DEFAULT_LOC_REQ_TTL_S


# ---------------------------------------------------------------------------
# jobs.tick() clears stale locReqs
# ---------------------------------------------------------------------------


def test_clear_stale_loc_reqs_removes_only_old_rows():
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-15", "student")
    _make_pager_device("pgr-loc-16", "student")
    device1 = devices_store.get_device("pgr-loc-15")
    device2 = devices_store.get_device("pgr-loc-16")

    _routing, _broker, loc = _setup()
    loc.locate(requester_uid="mom", device=device1)
    loc.locate(requester_uid="mom", device=device2)
    _backdate_loc_req("pgr-loc-15", seconds_ago=location.DEFAULT_LOC_REQ_TTL_S + 1)

    cleared = location.clear_stale_loc_reqs()
    assert cleared == 1

    from app.db.firestore import get_db

    assert not get_db().collection("locReqs").document("pgr-loc-15").get().exists
    assert get_db().collection("locReqs").document("pgr-loc-16").get().exists


def test_tick_clears_stale_loc_reqs_and_a_fresh_locate_does_not_coalesce():
    from app import jobs
    from app.tasks import InlineTaskQueue

    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-17", "student")
    device = devices_store.get_device("pgr-loc-17")

    routing, broker, loc = _setup()
    first = loc.locate(requester_uid="mom", device=device)
    _backdate_loc_req("pgr-loc-17", seconds_ago=location.DEFAULT_LOC_REQ_TTL_S + 1)

    result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert result.locReqsCleared == 1

    second = loc.locate(requester_uid="mom", device=device)
    assert second.request_id != first.request_id
    assert len(broker.published) == 2


# ---------------------------------------------------------------------------
# Cell-tower location fallback (docs/PROTOCOL.md §13.2, this task)
# ---------------------------------------------------------------------------


def test_cell_answer_fulfils_loc_req_with_src_cell(monkeypatch: pytest.MonkeyPatch):
    """A `no_fix` + `cell` answer resolves to a position, fulfils the
    matching `loc_req`, and is stored exactly like a fix, `src:"cell"`."""
    monkeypatch.setattr(
        cellgeo, "resolve", lambda cell: cellgeo.CellFix(lat=1.5, lon=2.5, acc_m=1200, provider="google")
    )
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-cell-1", "student")
    device = devices_store.get_device("pgr-loc-cell-1")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)

    answer = _loc_env(req=outcome.request_id, err="no_fix", cell=_cell_info())
    location.ingest_loc("pgr-loc-cell-1", answer)

    loc_req_msg = messages_store.get_message(outcome.request_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "fulfilled"

    fixes = locations_store.list_locations("pgr-loc-cell-1")
    assert len(fixes) == 1
    assert fixes[0].src == "cell"
    assert fixes[0].lat == 1.5
    assert fixes[0].lon == 2.5
    assert fixes[0].accM == 1200

    thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    loc_msgs = [m for m in thread if m.kind == "loc"]
    assert len(loc_msgs) == 1
    assert loc_msgs[0].loc["src"] == "cell"
    assert loc_msgs[0].loc["lat"] == 1.5
    assert "err" not in loc_msgs[0].loc


def test_cell_unresolvable_records_last_cell_and_behaves_like_no_fix(
    monkeypatch: pytest.MonkeyPatch,
):
    """docs/PROTOCOL.md §13.2: "If it cannot be resolved, behave as today
    for no_fix, but record the raw cell and the time on the device
    document"."""
    monkeypatch.setattr(cellgeo, "resolve", lambda cell: None)
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-cell-2", "student")
    device = devices_store.get_device("pgr-loc-cell-2")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)

    answer = _loc_env(req=outcome.request_id, err="no_fix", cell=_cell_info(tac=999, ci=42))
    location.ingest_loc("pgr-loc-cell-2", answer)

    # Same as a plain no_fix answer -- no location fix stored.
    assert locations_store.list_locations("pgr-loc-cell-2") == []
    thread = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    loc_msgs = [m for m in thread if m.kind == "loc"]
    assert len(loc_msgs) == 1
    assert loc_msgs[0].loc == {"err": "no_fix"}

    device_after = devices_store.get_device("pgr-loc-cell-2")
    assert device_after.status.lastCell is not None
    assert device_after.status.lastCell.tac == 999
    assert device_after.status.lastCell.ci == 42


def test_cell_alongside_gnss_fix_is_only_recorded_gnss_wins(monkeypatch: pytest.MonkeyPatch):
    """docs/PROTOCOL.md §13.2: "It may also send it alongside a real GNSS
    fix (then the GNSS fix wins and the cell is only recorded)." -- the
    resolver must not even be called."""
    called = {"n": 0}

    def _resolve(cell):
        called["n"] += 1
        return cellgeo.CellFix(lat=9.0, lon=9.0, acc_m=100, provider="google")

    monkeypatch.setattr(cellgeo, "resolve", _resolve)
    _make_user("student", "student")
    _make_pager_device("pgr-loc-cell-3", "student")

    env = _loc_env(lat=37.0, lon=-122.0, cell=_cell_info())
    location.ingest_loc("pgr-loc-cell-3", env)

    assert called["n"] == 0
    fixes = locations_store.list_locations("pgr-loc-cell-3")
    assert len(fixes) == 1
    assert fixes[0].src == "gnss"
    assert fixes[0].lat == 37.0

    device_after = devices_store.get_device("pgr-loc-cell-3")
    assert device_after.status.lastCell is not None  # recorded anyway


def test_cell_absent_is_backward_compatible_no_lastcell_written():
    """A pager that never sends `cell` (today's firmware) leaves
    `status.lastCell` untouched."""
    _make_user("student", "student")
    _make_pager_device("pgr-loc-cell-4", "student")

    location.ingest_loc("pgr-loc-cell-4", _loc_env())

    device_after = devices_store.get_device("pgr-loc-cell-4")
    assert device_after.status.lastCell is None


def test_cell_resolver_failure_does_not_break_ingest(monkeypatch: pytest.MonkeyPatch):
    """A bug or outage in the third-party resolver must never break `/loc`
    ingest -- the envelope is still processed exactly like a plain
    `no_fix`."""

    def _boom(cell):
        raise RuntimeError("simulated resolver crash")

    monkeypatch.setattr(cellgeo, "resolve", _boom)
    _make_user("mom", "mom")
    _make_user("student", "student")
    allow_store.set_edge("mom", "student", message=True, locate=True)
    _make_pager_device("pgr-loc-cell-5", "student")
    device = devices_store.get_device("pgr-loc-cell-5")

    _routing, _broker, loc = _setup()
    outcome = loc.locate(requester_uid="mom", device=device)

    answer = _loc_env(req=outcome.request_id, err="no_fix", cell=_cell_info())
    location.ingest_loc("pgr-loc-cell-5", answer)  # must not raise

    loc_req_msg = messages_store.get_message(outcome.request_id)
    pager_bid = next(bid for bid, d in loc_req_msg.deliveries.items() if d.kind == "pager")
    assert loc_req_msg.deliveries[pager_bid].state == "fulfilled"
    assert locations_store.list_locations("pgr-loc-cell-5") == []


def test_cell_periodic_unsolicited_answer_resolves_and_stores(monkeypatch: pytest.MonkeyPatch):
    """A periodic (`req: null`) `no_fix` + `cell` answer is resolved and
    stored the same way as an on-demand one."""
    monkeypatch.setattr(
        cellgeo, "resolve", lambda cell: cellgeo.CellFix(lat=4.0, lon=5.0, acc_m=800, provider="google")
    )
    _make_user("student", "student")
    _make_pager_device("pgr-loc-cell-6", "student")

    env = _loc_env(err="no_fix", cell=_cell_info())
    location.ingest_loc("pgr-loc-cell-6", env)

    fixes = locations_store.list_locations("pgr-loc-cell-6")
    assert len(fixes) == 1
    assert fixes[0].src == "cell"
    assert fixes[0].reqId is None

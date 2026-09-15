"""Unit tests for `app.routing.Routing`: allow-list enforcement, recipient
resolution (explicit alias / device default / broadcast), origin-backend
exclusion, the drop+system-reply path, and inline delivery via the fake
broker -- docs/SERVER_PLAN.md §5.2, §5.4.
"""

from __future__ import annotations

import json

import pytest

from app.routing import RejectedRecipient, Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def routing(broker: FakeBrokerClient) -> Routing:
    return Routing(broker)


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


def _make_pager_device(device_id: str, owner_uid: str, *, default_to_uid: str | None = None):
    # S1.4: this module's tests exercise fan-out/allow-list/redelivery logic,
    # not device signing -- `auth_mode="password"` keeps `publish_down`
    # (`app.broker`) publishing plain JSON, same as before that task, so the
    # `json.loads(broker.published[...].payload)` assertions below still
    # apply. `tests/test_ingest.py`'s `_make_hmac_pager_device` is the
    # signed-envelope counterpart.
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        default_to_uid=default_to_uid,
        auth_mode="password",
    )
    return backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )


# ---- allow-list enforcement ----


def test_send_to_disallowed_recipient_is_dropped_and_rejected(routing: Routing):
    _make_user("alice", "alice")
    _make_user("bob", "bob")
    # No allow edge between alice and bob.

    result = routing.send(
        sender_uid="alice", recipient_alias="bob", kind="text", body="hi", origin_backend_kind="webapp"
    )
    assert result.messages == []
    assert len(result.rejected) == 1
    assert result.rejected[0].reason == "not_allowed"
    assert result.rejected[0].uid == "bob"


def test_send_to_unknown_alias_is_rejected(routing: Routing):
    _make_user("alice", "alice")
    result = routing.send(
        sender_uid="alice",
        recipient_alias="nobody",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert result.messages == []
    assert result.rejected == [RejectedRecipient(alias="nobody", uid=None, reason="unknown_alias")]


def test_send_to_allowed_recipient_creates_message(routing: Routing):
    _make_user("alice", "alice")
    _make_user("bob", "bob")
    allow_store.set_edge("alice", "bob", message=True, locate=True)

    result = routing.send(
        sender_uid="alice", recipient_alias="bob", kind="text", body="hi", origin_backend_kind="webapp"
    )
    assert result.rejected == []
    assert len(result.messages) == 1
    assert result.messages[0].senderUid == "alice"
    assert result.messages[0].recipientUid == "bob"
    assert result.messages[0].body == "hi"


# ---- origin-backend exclusion ----


def test_self_loop_excludes_same_kind_delivery(routing: Routing, broker):
    """The one case `origin_backend` exclusion actually fires: a send whose
    resolved recipient is the sender themselves (a misconfigured
    self-referential allow edge, standing in for the degenerate cases the
    spec's "back to the sender's own device"/"same phone" wording
    describes) does not hand the message straight back out over the same
    channel it (hypothetically) arrived on."""
    _make_user("loopy", "loopy")
    allow_store.set_edge("loopy", "loopy", message=True, locate=True)
    _make_pager_device("pgr-loopy", "loopy")

    result = routing.send(
        sender_uid="loopy",
        recipient_alias="loopy",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
    )
    assert len(result.messages) == 1
    msg = result.messages[0]
    kinds = {d.kind for d in msg.deliveries.values()}
    # pager (== origin_backend) excluded; webapp (implicit, different kind)
    # still delivered.
    assert kinds == {"webapp"}
    assert broker.published == []


def test_origin_backend_id_scopes_self_loop_exclusion_to_exact_backend(routing: Routing, broker):
    """S2a: once a caller knows the *specific* origin backend document (the
    shape the SMS/gchat adapters pass), the self-loop guard must
    exclude only that exact backend id -- not every backend sharing its
    kind. A user with two pager devices (two phones, the motivating
    case) replying to themself over device A must still receive the
    message on device B."""
    _make_user("loopy2", "loopy2")
    allow_store.set_edge("loopy2", "loopy2", message=True, locate=True)
    backend_a = _make_pager_device("pgr-loopy2-a", "loopy2")
    backend_b = _make_pager_device("pgr-loopy2-b", "loopy2")

    result = routing.send(
        sender_uid="loopy2",
        recipient_alias="loopy2",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
        origin_backend_id=backend_a.id,
    )
    assert len(result.messages) == 1
    msg = result.messages[0]
    assert msg.originBackendKind == "pager"
    assert msg.originBackendId == backend_a.id
    # backend_a excluded by exact id; backend_b (same kind, different id)
    # and webapp (different kind) still delivered.
    assert backend_a.id not in msg.deliveries
    assert backend_b.id in msg.deliveries
    assert {d.kind for d in msg.deliveries.values()} == {"pager", "webapp"}
    assert len(broker.published) == 1


def test_cross_user_pager_delivery_not_excluded_by_matching_origin_kind(
    routing: Routing, broker
):
    """The corrected (non-literal) reading: two *different* users sharing a
    backend kind is not a self-loop, so a recipient's own pager backend is
    delivered to even when the sender's message also originated over
    `pager` -- see this module's docstring for why a blanket same-kind
    exclusion was rejected (it broke webapp read receipts, the ordinary
    parent<->student case)."""
    _make_user("alice", "alice")
    _make_user("bob", "bob")
    allow_store.set_edge("alice", "bob", message=True, locate=True)
    _make_pager_device("pgr-bob", "bob")

    result = routing.send(
        sender_uid="alice",
        recipient_alias="bob",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
    )
    msg = result.messages[0]
    kinds = {d.kind for d in msg.deliveries.values()}
    assert kinds == {"webapp", "pager"}
    assert len(broker.published) == 1


def test_non_matching_origin_backend_delivers_to_pager(routing: Routing, broker):
    _make_user("alice", "alice")
    _make_user("bob", "bob")
    allow_store.set_edge("alice", "bob", message=True, locate=True)
    _make_pager_device("pgr-bob3", "bob")

    result = routing.send(
        sender_uid="alice",
        recipient_alias="bob",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]
    kinds = {d.kind for d in msg.deliveries.values()}
    assert "pager" in kinds
    assert len(broker.published) == 1
    assert broker.published[0].topic == "pager/pgr-bob3/down"
    payload = json.loads(broker.published[0].payload)
    assert payload["id"] == msg.id
    assert payload["from"] == "alice"
    assert payload["body"] == "hi"

    # And the pager delivery was marked 'sent' by PagerBackend.deliver().
    refreshed = messages_store.get_message(msg.id)
    pager_bid = next(bid for bid, d in refreshed.deliveries.items() if d.kind == "pager")
    assert refreshed.deliveries[pager_bid].state == "sent"


# ---- recipient resolution: device default / broadcast ----


def test_no_alias_uses_device_default(routing: Routing, broker):
    _make_user("alice", "alice")
    _make_user("bob", "bob")
    allow_store.set_edge("bob", "alice", message=True, locate=True)
    _make_pager_device("pgr-bob4", "bob", default_to_uid="alice")

    result = routing.send(
        sender_uid="bob",
        recipient_alias=None,
        kind="text",
        body="hi",
        origin_backend_kind="pager",
        device_id="pgr-bob4",
    )
    assert len(result.messages) == 1
    assert result.messages[0].recipientUid == "alice"


def test_no_alias_no_default_broadcasts_to_all_allowed(routing: Routing, broker):
    _make_user("student", "student")
    _make_user("mom", "mom")
    _make_user("dad", "dad")
    allow_store.set_edge("student", "mom", message=True, locate=True)
    allow_store.set_edge("student", "dad", message=True, locate=True)
    _make_pager_device("pgr-student", "student", default_to_uid=None)

    result = routing.send(
        sender_uid="student",
        recipient_alias=None,
        kind="text",
        body="broadcast",
        origin_backend_kind="pager",
        device_id="pgr-student",
    )
    recipients = {m.recipientUid for m in result.messages}
    assert recipients == {"mom", "dad"}


# ---- wireId dedup across a broadcast fan-out ----


def test_wire_id_dedup_prevents_double_delivery_on_redelivery(routing: Routing):
    _make_user("student", "student")
    _make_user("mom", "mom")
    allow_store.set_edge("student", "mom", message=True, locate=True)

    first = routing.send(
        sender_uid="student",
        recipient_alias="mom",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
        wire_id="u_dupe1234",
    )
    assert len(first.messages) == 1

    # Same wire_id again (e.g. an at-least-once webhook redelivery).
    second = routing.send(
        sender_uid="student",
        recipient_alias="mom",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
        wire_id="u_dupe1234",
    )
    assert second.messages == []
    assert second.rejected == []

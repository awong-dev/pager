"""Unit tests for `app.routing.Routing`: allow-list enforcement, recipient
resolution (explicit alias / device default / broadcast), origin-backend
exclusion, the drop+system-reply path, and inline delivery via the fake
broker -- docs/SERVER_PLAN.md §5.2, §5.4.
"""

from __future__ import annotations

import json

import pytest

from app.backends.registry import build_registry
from app.routing import RejectedRecipient, Routing
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import push_tokens as push_tokens_store
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


def test_dm_pager_page_bytes_are_byte_identical_to_before_sndr(routing: Routing, broker):
    """docs/GROUP_CHAT_DESIGN.md §4: `sndr` is absent on every DM page, so a
    DM page's bytes are byte-for-byte what `PagerBackend` produced before
    `sndr` existed -- pinned as an exact comparison, not just a
    field-presence check, task G3's verify."""
    _make_user("dmalice", "dmalice")
    _make_user("dmbob", "dmbob")
    allow_store.set_edge("dmalice", "dmbob", message=True, locate=True)
    _make_pager_device("pgr-dmbob", "dmbob")

    result = routing.send(
        sender_uid="dmalice",
        recipient_alias="dmbob",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    msg = result.messages[0]
    assert len(broker.published) == 1
    expected = (
        f'{{"v":1,"id":"{msg.id}","ts":{msg.ts},"from":"dmalice","body":"hi","ack":null}}'
    ).encode()
    assert broker.published[0].payload == expected


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


# ---- group fan-out (docs/GROUP_CHAT_DESIGN.md §3) ----


def _all_edges(uids: list[str]) -> None:
    for a in uids:
        for b in uids:
            if a != b:
                allow_store.set_edge(a, b, message=True, locate=True)


def test_group_send_fans_out_to_every_member_but_sender(routing: Routing):
    _make_user("gowner", "gowner")
    _make_user("gmem1", "gmem1")
    _make_user("gmem2", "gmem2")
    _all_edges(["gowner", "gmem1", "gmem2"])
    group = conversations_store.create_group(
        name="Family",
        alias="fam-routing",
        member_uids=["gowner", "gmem1", "gmem2"],
        created_by="gowner",
    )

    result = routing.send(
        sender_uid="gowner",
        recipient_alias="fam-routing",
        kind="text",
        body="hi all",
        origin_backend_kind="webapp",
    )
    assert result.rejected == []
    assert len(result.messages) == 2
    recipients = {m.recipientUid for m in result.messages}
    assert recipients == {"gmem1", "gmem2"}
    assert "gowner" not in recipients

    # One seq, one groupMsgId, shared convKey -- every member's orderBy(seq)
    # agrees, and the web client can dedupe the sender's own N-1 copies.
    seqs = {m.seq for m in result.messages}
    group_msg_ids = {m.groupMsgId for m in result.messages}
    conv_keys = {m.convKey for m in result.messages}
    assert len(seqs) == 1
    assert len(group_msg_ids) == 1
    assert conv_keys == {group.convKey}
    for m in result.messages:
        assert m.senderAlias == "gowner"
        # The message document's own `uids` stays the sender/recipient pair,
        # never the full member list (docs/GROUP_CHAT_DESIGN.md §2).
        assert set(m.uids) == {"gowner", m.recipientUid}


def test_group_send_with_one_pager_publishes_one_page(routing: Routing, broker):
    _make_user("gowner2", "gowner2")
    _make_user("gmem3", "gmem3")
    _all_edges(["gowner2", "gmem3"])
    _make_pager_device("pgr-gmem3", "gmem3")
    conversations_store.create_group(
        name="Family", alias="fam-onepager", member_uids=["gowner2", "gmem3"], created_by="gowner2"
    )

    result = routing.send(
        sender_uid="gowner2",
        recipient_alias="fam-onepager",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert len(result.messages) == 1
    assert len(broker.published) == 1
    # What the page's `from`/`sndr` fields actually say is G3's concern
    # (docs/GROUP_CHAT_DESIGN.md §4, `relay/app/backends/pager.py`) -- this
    # test only pins the fan-out mechanics: a group with one pager member
    # produces exactly one page.


def test_group_send_with_two_pagers_publishes_two_pages_no_self_echo(routing: Routing, broker):
    _make_user("gowner3", "gowner3")
    _make_user("gpager_a_owner", "gpager_a_owner")
    _make_user("gpager_b_owner", "gpager_b_owner")
    members = ["gowner3", "gpager_a_owner", "gpager_b_owner"]
    _all_edges(members)
    _make_pager_device("pgr-a", "gpager_a_owner")
    _make_pager_device("pgr-b", "gpager_b_owner")
    conversations_store.create_group(
        name="Family", alias="fam-twopager", member_uids=members, created_by="gowner3"
    )

    result = routing.send(
        sender_uid="gowner3",
        recipient_alias="fam-twopager",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert len(result.messages) == 2
    assert len(broker.published) == 2
    topics = {p.topic for p in broker.published}
    # Neither pager device is itself the sender, so neither publish is an
    # echo of the sender's own message -- both group members with a pager
    # get their own page. (What each page's `from`/`sndr` fields say is
    # G3's concern, docs/GROUP_CHAT_DESIGN.md §4 / `app/backends/pager.py`.)
    assert topics == {"pager/pgr-a/down", "pager/pgr-b/down"}


def test_group_pager_page_carries_group_alias_as_from_and_sender_as_sndr(
    routing: Routing, broker
):
    """docs/GROUP_CHAT_DESIGN.md §4: `from` = the group's alias (the thread
    identity), `sndr` = the author's alias, task G3."""
    _make_user("gwireowner", "gwireowner")
    _make_user("gwiremem", "gwiremem")
    _all_edges(["gwireowner", "gwiremem"])
    _make_pager_device("pgr-gwire", "gwiremem")
    conversations_store.create_group(
        name="Family", alias="fam-wire", member_uids=["gwireowner", "gwiremem"], created_by="gwireowner"
    )

    routing.send(
        sender_uid="gwireowner",
        recipient_alias="fam-wire",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert len(broker.published) == 1
    payload = json.loads(broker.published[0].payload)
    assert payload["from"] == "fam-wire"
    assert payload["sndr"] == "gwireowner"
    assert payload["body"] == "hi"


def test_group_send_by_non_member_is_rejected(routing: Routing):
    _make_user("gowner4", "gowner4")
    _make_user("gmem4", "gmem4")
    _make_user("outsider", "outsider")
    _all_edges(["gowner4", "gmem4", "outsider"])
    conversations_store.create_group(
        name="Family", alias="fam-nonmember", member_uids=["gowner4", "gmem4"], created_by="gowner4"
    )

    result = routing.send(
        sender_uid="outsider",
        recipient_alias="fam-nonmember",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert result.messages == []
    assert len(result.rejected) == 1
    assert result.rejected[0].reason == "not_member"


def test_group_send_reposted_with_same_group_msg_id_creates_no_duplicates(routing: Routing):
    """Simulates the retry `create_message`'s per-recipient `wireIds` dedup
    is meant to repair: two `routing.send()` calls that happen to carry the
    same `wire_id` (e.g. a device's QoS-1 redelivery of the same `/up`
    envelope, `to: <group alias>`) create each recipient's copy at most
    once."""
    _make_user("gowner5", "gowner5")
    _make_user("gmem5", "gmem5")
    _make_user("gmem6", "gmem6")
    members = ["gowner5", "gmem5", "gmem6"]
    _all_edges(members)
    conversations_store.create_group(
        name="Family", alias="fam-dedupe", member_uids=members, created_by="gowner5"
    )

    first = routing.send(
        sender_uid="gowner5",
        recipient_alias="fam-dedupe",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
        wire_id="u_group_dupe",
    )
    assert len(first.messages) == 2

    second = routing.send(
        sender_uid="gowner5",
        recipient_alias="fam-dedupe",
        kind="text",
        body="hi",
        origin_backend_kind="pager",
        wire_id="u_group_dupe",
    )
    assert second.messages == []
    assert second.rejected == []


def test_group_send_missing_allow_edge_drops_only_that_recipient(routing: Routing, caplog):
    _make_user("gowner6", "gowner6")
    _make_user("gmem7", "gmem7")
    _make_user("gmem8", "gmem8")
    members = ["gowner6", "gmem7", "gmem8"]
    # Only gowner6 <-> gmem7 has an edge; gmem8 is missing one entirely
    # (as if an admin deleted it after the group was created).
    allow_store.set_edge("gowner6", "gmem7", message=True, locate=True)
    allow_store.set_edge("gmem7", "gowner6", message=True, locate=True)
    conversations_store.create_group(
        name="Family", alias="fam-missingedge", member_uids=members, created_by="gowner6"
    )

    with caplog.at_level("WARNING", logger="relay.routing"):
        result = routing.send(
            sender_uid="gowner6",
            recipient_alias="fam-missingedge",
            kind="text",
            body="hi",
            origin_backend_kind="webapp",
        )
    assert {m.recipientUid for m in result.messages} == {"gmem7"}
    assert len(result.rejected) == 1
    assert result.rejected[0].uid == "gmem8"
    assert result.rejected[0].reason == "not_allowed"
    assert any("SECURITY" in rec.message for rec in caplog.records)


# ---- push payload for a group send (docs/GROUP_CHAT_DESIGN.md §6), task G4 ----


class _RecordingFCMClient:
    def __init__(self) -> None:
        self.calls: list[tuple[list[str], dict[str, str]]] = []

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None:
        self.calls.append((tokens, data))


def test_group_send_push_payload_uses_group_name_alias_and_collapse_key(broker: FakeBrokerClient):
    _make_user("gpushowner", "gpushowner")
    _make_user("gpushmem", "gpushmem")
    _all_edges(["gpushowner", "gpushmem"])
    push_tokens_store.add_token("gpushmem", "tok_gpushmem")
    conv = conversations_store.create_group(
        name="Family", alias="fam-push-route", member_uids=["gpushowner", "gpushmem"], created_by="gpushowner"
    )

    fcm = _RecordingFCMClient()
    routing_with_fcm = Routing(broker, registry=build_registry(broker, fcm_client=fcm))
    routing_with_fcm.send(
        sender_uid="gpushowner",
        recipient_alias="fam-push-route",
        kind="text",
        body="hi family",
        origin_backend_kind="pager",
    )

    assert len(fcm.calls) == 1
    tokens, data = fcm.calls[0]
    assert tokens == ["tok_gpushmem"]
    assert data["convKey"] == conv.convKey
    assert data["senderAlias"] == "gpushowner"
    assert data["title"] == "Family"
    assert data["url"] == "/chat/fam-push-route"
    assert data["groupMsgId"]  # non-empty, shared with the pager copy's wireId

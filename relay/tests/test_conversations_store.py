"""Direct unit tests of `app.store.conversations` and the group-aware parts
of `app.store.messages` against the Firestore emulator --
docs/GROUP_CHAT_DESIGN.md §2, §3, task G1."""

from __future__ import annotations

import pytest
from google.api_core.exceptions import AlreadyExists

from app.store import conversations as conversations_store
from app.store import messages as messages_store
from app.store import users as users_store


def _make_user(uid: str, alias: str) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias)


def test_create_group_returns_conversation_with_sorted_members():
    _make_user("carol", "carol")
    _make_user("alice", "alice")
    _make_user("bob", "bob")

    conv = conversations_store.create_group(
        name="Family",
        alias="family",
        member_uids=["carol", "alice", "bob", "alice"],
        created_by="alice",
    )

    assert conv.kind == "group"
    assert conv.name == "Family"
    assert conv.alias == "family"
    assert conv.createdBy == "alice"
    assert conv.uids == ["alice", "bob", "carol"]
    assert conv.convKey.startswith("g_")


def test_create_group_alias_collision_raises_already_exists():
    _make_user("alice2", "alice2")
    _make_user("bob2", "bob2")
    conversations_store.create_group(
        name="Family", alias="dupe-group", member_uids=["alice2", "bob2"], created_by="alice2"
    )

    with pytest.raises(AlreadyExists):
        conversations_store.create_group(
            name="Family Again",
            alias="dupe-group",
            member_uids=["alice2", "bob2"],
            created_by="alice2",
        )


def test_create_group_alias_collides_with_user_alias():
    """The alias namespace is shared (docs/GROUP_CHAT_DESIGN.md §2): a group
    can't steal an existing user's alias, or vice versa."""
    _make_user("dave", "dave")
    _make_user("erin", "erin")

    with pytest.raises(AlreadyExists):
        conversations_store.create_group(
            name="Dave's group", alias="dave", member_uids=["dave", "erin"], created_by="dave"
        )


def test_create_group_requires_two_distinct_members():
    _make_user("solo", "solo")
    with pytest.raises(ValueError):
        conversations_store.create_group(
            name="Solo", alias="solo-group", member_uids=["solo", "solo"], created_by="solo"
        )


def test_get_by_alias_roundtrip():
    _make_user("frank", "frank")
    _make_user("gina", "gina")
    created = conversations_store.create_group(
        name="Fam", alias="famalias", member_uids=["frank", "gina"], created_by="frank"
    )

    fetched = conversations_store.get_by_alias("famalias")
    assert fetched is not None
    assert fetched.convKey == created.convKey
    assert fetched.kind == "group"


def test_get_by_alias_none_for_unknown_alias():
    assert conversations_store.get_by_alias("nope-does-not-exist") is None


def test_get_by_alias_none_for_user_alias():
    """A user's `aliases/{alias}` doc has a `uid` field, not `convKey` --
    resolving it through the group lookup must not accidentally succeed."""
    _make_user("harry", "harry")
    assert conversations_store.get_by_alias("harry") is None


def test_add_member_then_remove_member():
    _make_user("iris", "iris")
    _make_user("jack", "jack")
    _make_user("kate", "kate")
    conv = conversations_store.create_group(
        name="Trio", alias="trio", member_uids=["iris", "jack"], created_by="iris"
    )

    assert conversations_store.is_member(conv.convKey, "kate") is False
    added = conversations_store.add_member(conv.convKey, "kate")
    assert added.uids == ["iris", "jack", "kate"]
    assert conversations_store.is_member(conv.convKey, "kate") is True

    # Adding an existing member is a no-op, not an error or a duplicate.
    again = conversations_store.add_member(conv.convKey, "kate")
    assert again.uids == ["iris", "jack", "kate"]

    removed = conversations_store.remove_member(conv.convKey, "jack")
    assert removed.uids == ["iris", "kate"]
    assert conversations_store.is_member(conv.convKey, "jack") is False


def test_add_member_unknown_conversation_raises_key_error():
    with pytest.raises(KeyError):
        conversations_store.add_member("g_doesnotexist", "someone")


# ---------------------------------------------------------------------------
# create_message's group-aware keyword arguments (docs/GROUP_CHAT_DESIGN.md
# §3) -- byte-for-byte DM behaviour is asserted here at the store layer;
# G3 separately asserts it at the wire layer.
# ---------------------------------------------------------------------------


def test_dm_create_message_output_unchanged_field_for_field():
    """No group keyword argument given -> every field is exactly what
    today's DM call produces, including the new `groupMsgId`/`senderAlias`
    fields both being `None`."""
    msg = messages_store.create_message(
        sender_uid="mom", recipient_uid="kid", kind="text", ts=1000, body="hi"
    )
    assert msg is not None
    assert msg.convKey == messages_store.conv_key("mom", "kid")
    assert msg.uids == sorted(["mom", "kid"])
    assert msg.groupMsgId is None
    assert msg.senderAlias is None

    conv = messages_store.get_conversation(msg.convKey)
    assert conv is not None
    assert conv.kind == "dm"
    assert conv.uids == sorted(["mom", "kid"])


def test_group_copies_share_one_seq_and_group_msg_id():
    _make_user("owner1", "owner1")
    _make_user("member1", "member1")
    _make_user("member2", "member2")
    conv = conversations_store.create_group(
        name="G",
        alias="g-seq-test",
        member_uids=["owner1", "member1", "member2"],
        created_by="owner1",
    )

    seq = messages_store.allocate_seq()
    group_msg_id = "gm_test1"

    copy_a = messages_store.create_message(
        sender_uid="owner1",
        recipient_uid="member1",
        kind="text",
        ts=1000,
        body="hello group",
        conv_key=conv.convKey,
        uids=sorted(["owner1", "member1"]),
        seq=seq,
        group_msg_id=group_msg_id,
        sender_alias="owner1",
    )
    copy_b = messages_store.create_message(
        sender_uid="owner1",
        recipient_uid="member2",
        kind="text",
        ts=1000,
        body="hello group",
        conv_key=conv.convKey,
        uids=sorted(["owner1", "member2"]),
        seq=seq,
        group_msg_id=group_msg_id,
        sender_alias="owner1",
    )

    assert copy_a is not None
    assert copy_b is not None
    assert copy_a.seq == copy_b.seq == seq
    assert copy_a.groupMsgId == copy_b.groupMsgId == group_msg_id
    assert copy_a.convKey == copy_b.convKey == conv.convKey
    # Each copy's own `uids` field stays the sender/recipient pair, per §2 --
    # never the full member list (that lives on the conversation doc only).
    assert copy_a.uids == sorted(["owner1", "member1"])
    assert copy_b.uids == sorted(["owner1", "member2"])

    # The conversation's member list must survive being on the receiving
    # end of two `create_message` calls -- this is the bug the "don't
    # overwrite uids on update" fix in `create_message` guards against.
    conv_after = messages_store.get_conversation(conv.convKey)
    assert conv_after is not None
    assert conv_after.uids == ["member1", "member2", "owner1"]
    assert conv_after.unread["member1"] == 1
    assert conv_after.unread["member2"] == 1
    assert "owner1" not in conv_after.unread

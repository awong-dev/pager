"""Direct unit tests of `app.store.messages` against the Firestore emulator:
`create_message`'s transactional `wireId` dedup invariant (docs/SERVER_PLAN.md
§3, `app/store/messages.py`'s module docstring).

A real-concurrency version of this file (N threads racing on the same hot
`settings/meta.seqCounter` document) existed here and was removed: it could
only ever fail one of two ways -- a genuine correctness bug (duplicate/
missing `seq`), which any of its assertions would still catch instantly, or
`run_transaction`'s fixed outer-retry budget (app/db/firestore.py) running
out on a slow/loaded machine, which looks identical (a raised exception) but
is not a code defect -- confirmed by rerunning the exact same test locally
back-to-back with no code changes: every run passed, but wall-clock time
alone ranged from ~22s to ~53s, and it failed outright in CI's weaker/shared
runner. A CI gate that can go red with no corresponding code change is worse
than not having it: real regressions in `run_transaction`'s retry logic are
still exercised indirectly by every other test that calls `create_message`
(they all go through the same transaction/retry path), just not deliberately
raced."""

from __future__ import annotations

from app.db.firestore import get_db
from app.store import messages as messages_store
from app.store import users as users_store


def _count_messages() -> int:
    return len(list(get_db().collection("messages").stream()))


def _make_user(uid: str, alias: str, *, family_id: str | None = None) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=alias, family_id=family_id)


def test_create_message_same_wire_id_dedups():
    """Two `create_message` calls with the same `wireId` + recipient (the
    at-least-once webhook redelivery case, PROTOCOL.md): the second call
    returns `None` (dedup fired) rather than creating a second message, and
    the `messages` collection count stays at 1."""
    first = messages_store.create_message(
        sender_uid="alice",
        recipient_uid="bob",
        kind="text",
        ts=1000,
        body="hi",
        wire_id="w_dupe",
    )
    assert first is not None

    second = messages_store.create_message(
        sender_uid="alice",
        recipient_uid="bob",
        kind="text",
        ts=1001,
        body="hi again",
        wire_id="w_dupe",
    )
    assert second is None

    assert _count_messages() == 1


def test_create_message_wire_id_doc_carries_created_at():
    """`wireIds/{wireId}_{recipientUid}` must
    carry its own `createdAt` so `app/jobs.py`'s `sweep()` can reclaim it
    independently of its parent message's own deletion -- see that
    function's module docstring."""
    msg = messages_store.create_message(
        sender_uid="alice",
        recipient_uid="carol",
        kind="text",
        ts=1000,
        body="hi",
        wire_id="w_stamped",
    )
    assert msg is not None

    snap = get_db().collection("wireIds").document("w_stamped_carol").get()
    assert snap.exists
    assert snap.get("createdAt") is not None


# ---------------------------------------------------------------------------
# docs/FAMILIES_DESIGN.md §1 decisions 3-4, §3: `familyIds`/`participants`.
# ---------------------------------------------------------------------------


def test_create_message_sets_family_ids_across_two_families():
    """A DM between two different families' members carries both
    `familyId`s, sorted, on the message *and* on the lazily-created
    conversation doc -- and `participants` is populated for both sides."""
    _make_user("fam_a_1", "fam_a_1", family_id="fam_a")
    _make_user("fam_b_1", "fam_b_1", family_id="fam_b")

    msg = messages_store.create_message(
        sender_uid="fam_a_1",
        recipient_uid="fam_b_1",
        kind="text",
        ts=1000,
        body="hi",
    )
    assert msg is not None
    assert msg.familyIds == ["fam_a", "fam_b"]

    conv = messages_store.get_conversation(msg.convKey)
    assert conv is not None
    assert conv.familyIds == ["fam_a", "fam_b"]
    assert set(conv.participants) == {"fam_a_1", "fam_b_1"}
    assert conv.participants["fam_a_1"].alias == "fam_a_1"
    assert conv.participants["fam_b_1"].displayName == "fam_b_1"


def test_create_message_with_external_carries_only_the_member_family():
    """An external (`kind: 'external'`, `familyId: None`) contributes
    nothing to `familyIds` -- docs/FAMILIES_DESIGN.md §1 decision 6/§1
    decision 4's "an SMS participant has no family" case."""
    _make_user("fam_c_1", "fam_c_1", family_id="fam_c")
    users_store.create_user(
        uid="ext_1", alias="15551234567", display_name="15551234567", kind="external"
    )

    msg = messages_store.create_message(
        sender_uid="fam_c_1",
        recipient_uid="ext_1",
        kind="text",
        ts=1000,
        body="hi",
    )
    assert msg is not None
    assert msg.familyIds == ["fam_c"]

    conv = messages_store.get_conversation(msg.convKey)
    assert conv is not None
    assert conv.familyIds == ["fam_c"]


def test_create_message_does_not_rewrite_participants_on_an_existing_conversation():
    """Per docs/FAMILIES_DESIGN.md §3's trigger list, `participants` is
    written on conversation *creation* only -- an ordinary second message
    must not touch it even if a participant's displayName has since
    changed."""
    _make_user("fam_d_1", "fam_d_1", family_id="fam_d")
    _make_user("fam_d_2", "fam_d_2", family_id="fam_d")

    first = messages_store.create_message(
        sender_uid="fam_d_1", recipient_uid="fam_d_2", kind="text", ts=1000, body="hi"
    )
    assert first is not None
    users_store.update_user("fam_d_2", display_name="renamed")

    second = messages_store.create_message(
        sender_uid="fam_d_1", recipient_uid="fam_d_2", kind="text", ts=1001, body="hi again"
    )
    assert second is not None

    conv = messages_store.get_conversation(first.convKey)
    assert conv is not None
    assert conv.participants["fam_d_2"].displayName == "fam_d_2"

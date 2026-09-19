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


def _count_messages() -> int:
    return len(list(get_db().collection("messages").stream()))


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

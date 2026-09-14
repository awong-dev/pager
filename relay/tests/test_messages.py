"""Direct unit tests of `app.store.messages` against the Firestore emulator:
`create_message`'s two transactional invariants (docs/SERVER_PLAN.md §3,
`app/store/messages.py`'s module docstring) -- `wireId` dedup and the
monotonic `seqCounter` under real concurrency."""

from __future__ import annotations

import threading

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


def test_concurrent_create_message_seq_all_distinct():
    """N threads calling `create_message` concurrently (different
    wireIds/recipients, so nothing dedups against anything else) all
    contend on the same hot `settings/meta.seqCounter` document. All
    resulting `seq` values must be distinct, no thread may raise, and the
    total count of created messages must match the thread count."""
    n = 12
    results: list[messages_store.Message] = []
    errors: list[BaseException] = []
    barrier = threading.Barrier(n)

    def worker(i: int) -> None:
        barrier.wait()
        try:
            msg = messages_store.create_message(
                sender_uid=f"sender-{i}",
                recipient_uid=f"recipient-{i}",
                kind="text",
                ts=1000 + i,
                body=f"hi from {i}",
                wire_id=f"w_{i}",
            )
            assert msg is not None
            results.append(msg)
        except BaseException as exc:  # noqa: BLE001 -- captured for the assertion below
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert errors == []
    assert len(results) == n
    seqs = [m.seq for m in results]
    assert len(set(seqs)) == n
    assert _count_messages() == n

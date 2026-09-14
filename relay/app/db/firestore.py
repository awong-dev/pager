"""firebase-admin / Cloud Firestore init, emulator-aware.

docs/SERVER_PLAN.md §2 decision 3: **the relay is the only writer** to
Firestore -- it always goes in through the firebase-admin SDK (which
bypasses `firestore.rules` entirely; those rules exist to bound *client*
reads, see `relay/firestore.rules`). This module owns the one
`firebase_admin.App` / `google.cloud.firestore.Client` the whole process
uses, plus the transaction-helper pattern every `app/store/*.py` module
reuses for its monotonic/unique invariants (`seqCounter`, `wireIds` dedup,
`aliases` uniqueness).

Emulator-aware: when `FIRESTORE_EMULATOR_HOST` / `FIREBASE_AUTH_EMULATOR_HOST`
are set (docker-compose.yml, tests/conftest.py), the underlying
`google-cloud-firestore` client and `firebase_admin.auth` both already know
to redirect there -- that part needs no code here. What *does* need code:
`firebase_admin.initialize_app()` still wants a credential object, and the
default `credentials.ApplicationDefault()` calls `google.auth.default()`,
which walks a real ADC lookup chain (env var, gcloud config, GCE metadata
server) and raises if none of that exists, which is exactly the case in a
container that only has emulators and a `demo-` project id. `EmulatorCredentials`
below is a trivial `firebase_admin.credentials.Base` that hands back
`google.auth.credentials.AnonymousCredentials()` -- valid enough to
construct the SDK, and never actually presented to anything real because
every emulated call short-circuits on the `*_EMULATOR_HOST` env vars before
it would need a bearer token.
"""

from __future__ import annotations

import os
import random
import threading
import time
from collections.abc import Callable
from typing import TypeVar

import firebase_admin
from firebase_admin import credentials
from google.api_core.exceptions import Aborted
from google.auth.credentials import AnonymousCredentials
from google.cloud.firestore import Client, DocumentReference, Transaction, transactional

T = TypeVar("T")

# google.cloud.firestore_v1.transaction.MAX_ATTEMPTS (5) is the *client
# library's own* retry budget for one `db.transaction()` object on
# contention (ABORTED); it retries with no backoff ("preserves the
# transaction's spot in line"), which is right for a single caller but can
# still be exhausted when two callers are hammering the *same* hot document
# (e.g. `settings/meta.seqCounter` -- every message create touches it) at
# almost exactly the same instant. `run_transaction` below adds a small
# number of outer retries, each on a *fresh* transaction (a fresh "spot in
# line"), with full-jitter *exponential* backoff between them (AWS's
# "full jitter": `random.uniform(0, min(cap, base * 2**attempt))`) --
# linear backoff wasn't enough headroom to reliably absorb a dozen-way
# concurrent burst against the local emulator (still ~1/3 failure rate);
# exponential spreads later retries out enough for the queue of waiters on
# the hot document to actually drain. Worst-case total sleep across all
# attempts (each capped at `EXTRA_RETRY_MAX_DELAY_S`) is ~6.3s with the
# constants below, bounded well under `tools/emqx_setup.py`'s 10s webhook
# `request_ttl` (the local dev/test contention this budget exists for is not
# the low-thousands-of-ops-per-*day* household scale of
# docs/SERVER_PLAN.md §9.3, so this is deliberately generous rather than
# tightly tuned).
EXTRA_RETRY_ATTEMPTS = 6
EXTRA_RETRY_BASE_DELAY_S = 0.1
EXTRA_RETRY_MAX_DELAY_S = 3.2

_lock = threading.Lock()
_app: firebase_admin.App | None = None
_db: Client | None = None


class EmulatorCredentials(credentials.Base):
    """Satisfies `firebase_admin.initialize_app`'s credential requirement
    without touching real Application Default Credentials. Only ever
    installed when an emulator host env var is present (see `get_app`)."""

    def get_credential(self) -> AnonymousCredentials:
        return AnonymousCredentials()


def _using_emulators() -> bool:
    return bool(
        os.environ.get("FIRESTORE_EMULATOR_HOST") or os.environ.get("FIREBASE_AUTH_EMULATOR_HOST")
    )


def get_app() -> firebase_admin.App:
    """The process-wide `firebase_admin.App`, created on first use. Safe to
    call from any thread/request; FastAPI's lifespan does not need to own
    this explicitly, but `create_app`'s lifespan calls it anyway so
    misconfiguration (e.g. no project id and no emulator) fails at startup
    rather than on the first request."""
    global _app
    with _lock:
        if _app is not None:
            return _app
        project_id = os.environ.get("GOOGLE_CLOUD_PROJECT") or os.environ.get("GCLOUD_PROJECT")
        options: dict[str, str] = {"projectId": project_id} if project_id else {}
        if _using_emulators():
            cred: credentials.Base = EmulatorCredentials()
        else:
            cred = credentials.ApplicationDefault()
        _app = firebase_admin.initialize_app(cred, options)
        return _app


def get_db() -> Client:
    """The process-wide Firestore client. `google.cloud.firestore` reads
    `FIRESTORE_EMULATOR_HOST` itself (no special-casing needed here) and
    otherwise talks to the real service using the app's credentials."""
    global _db
    with _lock:
        if _db is not None:
            return _db
    get_app()  # ensure initialized (and fail fast if misconfigured)
    from firebase_admin import firestore as fa_firestore

    with _lock:
        if _db is None:
            _db = fa_firestore.client()
        return _db


def reset_for_tests() -> None:
    """Tears down the cached app/client so a fresh process-wide singleton is
    created on next use. Only meaningful in tests, which may re-point
    `FIRESTORE_EMULATOR_HOST` between runs or want a clean slate; production
    code never calls this."""
    global _app, _db
    with _lock:
        if _app is not None:
            try:
                firebase_admin.delete_app(_app)
            except ValueError:
                pass
        _app = None
        _db = None


def run_transaction[T](fn: Callable[[Transaction], T]) -> T:
    """Runs `fn(transaction)` inside a Firestore transaction. `fn` MUST only
    read with `transaction.get(...)` and stage writes with `transaction.set`
    / `.update` / `.create` -- never `DocumentReference.get()`/`.set()`
    directly, or the transaction loses its isolation. This is the one
    pattern `app/store/*.py` reuses for `settings/meta.seqCounter` and the
    `wireIds`/`aliases` dedup-by-create invariants (docs/SERVER_PLAN.md §3).

    Adds `EXTRA_RETRY_ATTEMPTS` outer retries (fresh transaction each time,
    jittered backoff) on top of the client library's own 5-attempt,
    no-backoff retry of a *single* transaction object, for the hot-document
    contention case described above `EXTRA_RETRY_ATTEMPTS`.

    Two shapes of contention have to be caught, and they are NOT the same:

    * `ValueError("Failed to commit transaction in N attempts.")` -- the
      client library's own wrapper, raised when its inner loop exhausts
      itself on `ABORTED` *at commit*. It chains the losing `Aborted` as
      `__cause__`; we key on that rather than on a substring of the message,
      so a plain `ValueError` raised by `fn` itself is never silently
      retried.
    * A bare `Aborted` ("Transaction lock timeout") raised by a
      `transaction.get(...)` *read*. The library's loop only wraps
      `_commit()`, so an `ABORTED` during the read phase propagates straight
      out of `@transactional` with no retry at all -- which is exactly what a
      dozen concurrent `messages.create_message` calls on the hot
      `settings/meta` document produce. Without this arm, the caller sees a
      `GoogleAPICallError` and the webhook 500s.

    Anything else (including a genuine bug in `fn`) propagates on the first
    attempt, same as before this helper existed."""
    db = get_db()
    last_exc: BaseException | None = None
    for attempt in range(EXTRA_RETRY_ATTEMPTS + 1):
        transaction = db.transaction()

        @transactional
        def _run(txn: Transaction) -> T:
            return fn(txn)

        try:
            return _run(transaction)
        except Aborted as exc:
            last_exc = exc
        except ValueError as exc:
            if not isinstance(exc.__cause__, Aborted):
                raise
            last_exc = exc
        if attempt < EXTRA_RETRY_ATTEMPTS:
            cap = min(EXTRA_RETRY_MAX_DELAY_S, EXTRA_RETRY_BASE_DELAY_S * (2**attempt))
            time.sleep(random.uniform(0, cap))
    assert last_exc is not None
    raise last_exc


def doc_exists_in_txn(transaction: Transaction, ref: DocumentReference) -> bool:
    """Small helper: `True` if `ref` already has a document, read inside
    `transaction` (so the check is part of the transaction's read set)."""
    snap = ref.get(transaction=transaction)
    return snap.exists

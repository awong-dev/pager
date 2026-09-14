"""Firestore-backed rate limiting -- docs/SERVER_PLAN.md Phase 8 hardening
punch list item 2 (server-architect's Phase 7 review).

A fixed-window counter keyed by an arbitrary caller-chosen string
(`"backends:{uid}"`, `"admin:{uid}"`, `"webhook_ip:sms:{ip}"`, ...), stored at
`rateLimits/{key}` as `{windowStart: epoch_s, count: int}`. Chosen over a
sliding-window log (one document per request) or an in-memory counter for two
reasons: (1) the relay is a scale-to-zero, multi-instance Cloud Run service
(docs/SERVER_PLAN.md §2 decision 1/§9.2) with no shared memory between
instances or across a cold start, so an in-process counter would silently
reset/fragment and give no real bound at all; (2) a single document per key,
updated in a transaction, is exactly the "monotonic, must be race-free" shape
this codebase already uses transactions for everywhere else (§3's habit:
"transactions for anything monotonic or unique", `app/db/firestore.py`'s
`run_transaction`). The one accepted weakness of a *fixed* (not sliding)
window -- a caller can do `limit` requests right at the end of one window and
another `limit` right at the start of the next, i.e. up to ~2x `limit` in a
short burst around the boundary -- is fine for every use of this module
today: `POST /api/me/backends` (bounds real-SMS-sending abuse, not a
precision billing control), `/api/admin/*` writes (defense-in-depth on an
already auth-gated surface), and per-IP webhook caps (defense-in-depth on an
already signature/JWT-gated surface per each webhook's own auth check) --
none of them need sliding-window precision, and a sliding-window log would
cost one Firestore document *per request* for a limiter that
`tools/e2e_v2.py`'s admin-heavy scenarios alone call 30+ times in one run.
"""

from __future__ import annotations

import time

from google.cloud.firestore import Transaction

from app.db.firestore import get_db, run_transaction


def _rate_limits():
    return get_db().collection("rateLimits")


def check_and_increment(key: str, *, limit: int, window_s: int, now: float | None = None) -> bool:
    """Returns True (and records this attempt) if `key` has made fewer than
    `limit` calls in the current `window_s`-second fixed window; returns
    False (and does NOT record -- a *rejected* call must never itself count
    against the caller, or a client retrying a 429 would only ever dig itself
    deeper) otherwise.

    `now` is injectable for tests only (asserting "trips after N, resets
    after the window" without a real sleep) -- every real caller leaves it as
    the current wall clock."""
    now = now if now is not None else time.time()
    ref = _rate_limits().document(key)

    def _txn(transaction: Transaction) -> bool:
        snap = ref.get(transaction=transaction)
        data = snap.to_dict() if snap.exists else None
        window_start = data.get("windowStart") if data else None
        count = data.get("count", 0) if data else 0
        if window_start is None or (now - window_start) >= window_s:
            # No window yet, or the previous one has fully elapsed -- this
            # call starts a fresh one.
            transaction.set(ref, {"windowStart": now, "count": 1})
            return True
        if count >= limit:
            return False
        transaction.update(ref, {"count": count + 1})
        return True

    return run_transaction(_txn)

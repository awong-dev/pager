"""Delivery-retry task queue abstraction -- docs/SERVER_PLAN.md §5's
`tasks.py` line ("Cloud Tasks enqueue (prod) / inline thread (dev) for
delivery retries") and `HANDOFF_V2.md` §4's `TASKS_MODE` compose env var.

Only `inline` mode is implemented this phase: `InlineTaskQueue.enqueue(fn,
...)` just calls `fn()` synchronously, right now, in the caller's own
request/tick. There is deliberately no backoff-and-retry loop of its own
here -- docs/SERVER_PLAN.md §5.2's "backoff 30 s ... 15 min, max 5" describes
a *real* Cloud Tasks queue's behaviour, which does not exist locally; a task
that raises inline is logged and dropped, and the next natural trigger
(`/internal/tick`, or the device's next online edge) is what retries it.

Kept behind a `TaskQueue` Protocol precisely so a later phase can add a real
`CloudTasksQueue` (enqueueing an HTTP task at `POST /internal/task`, per
docs/SERVER_PLAN.md §5.1) selected by `TASKS_MODE=cloud_tasks` without
changing any caller -- `app/jobs.py`'s `tick()` is the one caller this phase
has, and it only ever talks to this module through `build_task_queue()`.
"""

from __future__ import annotations

import logging
import os
from collections.abc import Callable
from typing import Protocol

logger = logging.getLogger("relay.tasks")


class TaskQueue(Protocol):
    def enqueue(self, fn: Callable[[], None], *, name: str) -> None: ...


class InlineTaskQueue:
    """`TASKS_MODE=inline` -- the only mode implemented this phase. Runs
    `fn` synchronously and swallows (logs) any exception, so one bad retry
    can never take the rest of a tick down with it."""

    def enqueue(self, fn: Callable[[], None], *, name: str) -> None:
        try:
            fn()
        except Exception:
            logger.exception("inline task %r raised", name)


def build_task_queue(mode: str | None = None) -> TaskQueue:
    """`mode` defaults to the `TASKS_MODE` env var (itself defaulting to
    `inline`, matching `relay/docker-compose.yml` / `HANDOFF_V2.md` §4).
    Any other value raises rather than silently falling back -- a
    misconfigured `TASKS_MODE` in a future prod deploy must fail loudly, not
    quietly run every retry inline."""
    mode = mode or os.environ.get("TASKS_MODE", "inline")
    if mode != "inline":
        raise NotImplementedError(
            f"TASKS_MODE={mode!r} is not implemented yet -- only 'inline' exists. "
            "A later phase adds a real Cloud Tasks client behind this same TaskQueue "
            "interface (docs/SERVER_PLAN.md §5's `tasks.py` line)."
        )
    return InlineTaskQueue()

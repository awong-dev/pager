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

Kept behind a `TaskQueue` Protocol so a real `CloudTasksQueue` (enqueueing an
HTTP task at `POST /internal/task`, per docs/SERVER_PLAN.md §5.1) can be
selected by `TASKS_MODE=cloud_tasks` without changing any caller --
`app/jobs.py`'s `tick()` is the one caller this phase has, and it only ever
talks to this module through `build_task_queue()`.

**`(build addition, phase 7)` `CloudTasksQueue` and its known gap**: every
`enqueue()` call site in this codebase today (`app/jobs.py`'s `tick()`,
`app/routing.py`'s retry paths) passes an in-process Python closure --
`lambda: routing.redeliver_pager(msg, device_id)` and similar -- because
`InlineTaskQueue.enqueue()` just calls it. A *real* Cloud Tasks queue is an
out-of-process HTTP dispatch: it cannot serialize and later invoke an
arbitrary closure, only POST a JSON body to a URL
(`docs/SERVER_PLAN.md`'s own `POST /internal/task`). So `CloudTasksQueue.
enqueue()` below does **not** attempt to run or serialize `fn` -- it builds
and creates a Cloud Tasks HTTP task carrying only `name` (the caller's own
opaque retry-identifier string) targeting `/internal/task`, and that is as
far as this phase's brief ("write a test that constructs the client and
confirms it builds the correct task payload/target URL, without actually
enqueueing anything") asks it to go. **Two things this leaves unfinished,
flagged rather than silently assumed:**
1. `POST /internal/task` itself does not exist yet (`app/routers/
   internal.py` only has `/tick` and `/sweep`) -- there is nowhere for the
   real Cloud Tasks dispatch to land, so `TASKS_MODE=cloud_tasks` is not
   actually usable end-to-end today.
2. Even once that route exists, `name` alone is not enough for it to
   reconstruct "retry `msg_id`'s delivery to `backend_id`" -- the
   `fn`-closure design `app/jobs.py`/`app/routing.py` use was never built
   to survive an out-of-process hop. Making `TASKS_MODE=cloud_tasks` fully
   load-bearing needs those call sites to enqueue a structured, replayable
   payload (e.g. `{"kind": "pager-retry", "msgId": ..., "deviceId": ...}`)
   instead of a closure -- a real (if mechanical) refactor of `app/jobs.py`
   and `app/routing.py`'s retry call sites, out of scope for this phase's
   narrower "add a real Cloud Tasks client, tested at construction" ask.
`TASKS_MODE=inline` (unchanged) is unaffected by any of this and stays the
only mode a real deployment needs until that follow-up lands.
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
    """`TASKS_MODE=inline` -- the default/dev mode. Runs `fn` synchronously
    and swallows (logs) any exception, so one bad retry can never take the
    rest of a tick down with it."""

    def enqueue(self, fn: Callable[[], None], *, name: str) -> None:
        try:
            fn()
        except Exception:
            logger.exception("inline task %r raised", name)


# ---------------------------------------------------------------------------
# CloudTasksQueue -- TASKS_MODE=cloud_tasks. See this module's docstring for
# what it does and does not do.
# ---------------------------------------------------------------------------


def _project() -> str:
    return os.environ.get("GOOGLE_CLOUD_PROJECT", "")


def _location() -> str:
    return os.environ.get("TASKS_LOCATION", "us-central1")


def _queue() -> str:
    return os.environ.get("TASKS_QUEUE", "relay-retries")


def _target_url() -> str:
    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    return f"{base}/internal/task"


def _service_account_email() -> str:
    return os.environ.get("TASKS_SERVICE_ACCOUNT_EMAIL", "")


def queue_path(*, project: str | None = None, location: str | None = None, queue: str | None = None) -> str:
    """`projects/{project}/locations/{location}/queues/{queue}` -- the
    `parent` `CloudTasksClient.create_task` needs."""
    project = project if project is not None else _project()
    location = location if location is not None else _location()
    queue = queue if queue is not None else _queue()
    return f"projects/{project}/locations/{location}/queues/{queue}"


def build_task(name: str) -> dict:
    """The `google.cloud.tasks_v2.types.Task`-shaped dict `CloudTasksQueue.
    enqueue()` sends -- factored out so a test can assert on the exact
    payload/target URL without needing a real Cloud Tasks queue to create a
    task against (per this phase's brief). An HTTP POST task with an OIDC
    token (docs/SERVER_PLAN.md §5.1: "`/internal/tick`, `/internal/sweep`,
    `/internal/task` ... OIDC token") -- Cloud Tasks mints and attaches the
    token itself at dispatch time from `oidc_token.service_account_email`,
    so no credential is embedded in the task body."""
    import json

    body = json.dumps({"name": name}).encode("utf-8")
    task: dict = {
        "http_request": {
            "http_method": "POST",
            "url": _target_url(),
            "headers": {"Content-Type": "application/json"},
            "body": body,
        }
    }
    sa_email = _service_account_email()
    if sa_email:
        task["http_request"]["oidc_token"] = {"service_account_email": sa_email}
    return task


class CloudTasksQueue:
    """`TASKS_MODE=cloud_tasks` -- enqueues a real Cloud Tasks HTTP task per
    `enqueue()` call rather than running anything inline. See this module's
    docstring for the known gap (no `/internal/task` handler yet, and
    `name` alone is not a replayable payload) -- this class only builds and
    creates the task; a failed `create_task` call is logged and swallowed,
    the same "never let a retry-scheduling failure raise into the caller's
    request" contract `InlineTaskQueue` already has."""

    def __init__(self, client: object | None = None) -> None:
        if client is None:
            from google.cloud import tasks_v2

            client = tasks_v2.CloudTasksClient()
        self._client = client

    def enqueue(self, fn: Callable[[], None], *, name: str) -> None:
        # `fn` is deliberately unused -- see this module's docstring's
        # "known gap" section. Kept as a parameter (rather than narrowing
        # the `TaskQueue` Protocol) so this class is still a drop-in
        # `TaskQueue` for every existing `enqueue(fn, name=...)` call site.
        del fn
        try:
            self._client.create_task(parent=queue_path(), task=build_task(name))
        except Exception:
            logger.exception("cloud tasks enqueue %r failed", name)


def build_task_queue(mode: str | None = None) -> TaskQueue:
    """`mode` defaults to the `TASKS_MODE` env var (itself defaulting to
    `inline`, matching `relay/docker-compose.yml` / `HANDOFF_V2.md` §4)."""
    mode = mode or os.environ.get("TASKS_MODE", "inline")
    if mode == "inline":
        return InlineTaskQueue()
    if mode == "cloud_tasks":
        return CloudTasksQueue()
    raise NotImplementedError(
        f"TASKS_MODE={mode!r} is not implemented -- only 'inline' and 'cloud_tasks' exist "
        "(docs/SERVER_PLAN.md §5's `tasks.py` line)."
    )

"""Structured JSON logging for Cloud Run/Cloud Logging.

Cloud Logging's structured-log ingestion parses each stdout/stderr line as a
JSON object and maps a top-level `severity` field
(`DEBUG`/`INFO`/`WARNING`/`ERROR`/`CRITICAL`) to its own log-level facet
(https://cloud.google.com/logging/docs/structured-logging) -- Python's own
`logging` level names already match those five strings for every level this
codebase actually uses (`logger.debug/info/warning/error/exception`), so
`JsonLogFormatter` below only needs to rename the field, not remap values.

A per-request correlation id (`X-Request-Id` if the caller sent one, else a
generated UUID) is threaded through every log line emitted while handling
that request via a `contextvars.ContextVar` -- the standard way to carry
per-request state through code that doesn't pass a `Request` object
explicitly at every call site (every `logger.info(...)` call in this
codebase today). A `ContextVar` (not thread-local storage) is required
specifically because `app/routers/webhooks.py`'s `/webhooks/mqtt` handler
runs its dispatch `off the event loop` via `starlette.concurrency.
run_in_threadpool` (see that module's own comment) -- `ContextVar` values
copy across that hop (`contextvars.copy_context()`, which
`run_in_threadpool`/`anyio` already use internally for exactly this reason);
thread-local storage would not survive it.
"""

from __future__ import annotations

import contextvars
import json
import logging
import time

request_id_var: contextvars.ContextVar[str | None] = contextvars.ContextVar(
    "request_id", default=None
)

_LEVEL_TO_SEVERITY: dict[int, str] = {
    logging.DEBUG: "DEBUG",
    logging.INFO: "INFO",
    logging.WARNING: "WARNING",
    logging.ERROR: "ERROR",
    logging.CRITICAL: "CRITICAL",
}


class JsonLogFormatter(logging.Formatter):
    """One JSON object per line: `severity`, `message`, `timestamp`, `logger`
    always; `requestId` when a request is in flight (`request_id_var`);
    `exception` (the formatted traceback) when the record carries one."""

    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, object] = {
            "severity": _LEVEL_TO_SEVERITY.get(record.levelno, record.levelname),
            "message": record.getMessage(),
            "timestamp": (
                time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(record.created))
                + f".{int(record.msecs):03d}Z"
            ),
            "logger": record.name,
        }
        request_id = request_id_var.get()
        if request_id is not None:
            payload["requestId"] = request_id
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        # `default=str`: a `%r`-formatted arg elsewhere in this codebase can
        # already contain arbitrary repr'd objects inside `message` (a plain
        # string by the time it gets here, via `record.getMessage()`), so
        # this is only a safety net for any future non-JSON-native value
        # landing directly in `payload` -- never raise out of a log call.
        return json.dumps(payload, default=str)


def configure_logging(level: str | int = "INFO") -> None:
    """Replaces the root logger's handlers with a single JSON-formatted
    stream handler. Idempotent -- clears any existing handlers first rather
    than stacking a duplicate -- so it's safe to call more than once (e.g.
    `app/main.py`'s module scope constructing more than one `FastAPI` app
    across a test session, each via `create_app()`)."""
    root = logging.getLogger()
    root.setLevel(level)
    handler = logging.StreamHandler()
    handler.setFormatter(JsonLogFormatter())
    root.handlers = [handler]

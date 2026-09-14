"""`POST /webhooks/mqtt` -- the broker's rule engine pushes device traffic
here instead of the relay holding an MQTT subscription (docs/SERVER_PLAN.md
§2 decision 1, §5.1). Auth is the shared-secret header
(`BrokerClient.verify_webhook`); dispatch is by topic suffix to
`app.ingest.Ingest`.

Per docs/PROTOCOL.md §3.4, this endpoint returns 200 for *almost* anything
-- a malformed payload is dropped (and logged) inside `ingest`, not
signalled with a non-2xx, so the broker's rule engine never retry-storms a
message it will never be able to make valid. The one thing that is NOT a
200 is a bad/missing webhook key: that is an auth failure, not a malformed
device payload, and the broker config itself should never produce it.

A `sqlite3.OperationalError` (DB locked / disk full / etc.) is also not a
200: §3.4's 2xx-for-malformed-payload rule only covers payloads the relay
will never be able to make valid, not genuine relay-side storage failures.
Swallowing those would silently lose an up-message with no republish path
(§4.2 has no up-message republish rule), so we let it become a 500 instead
and rely on the broker's rule engine to retry the webhook.
"""

from __future__ import annotations

import logging
import sqlite3
from collections.abc import Callable

from fastapi import APIRouter, HTTPException, Request, Response
from starlette.concurrency import run_in_threadpool

from app.broker import BrokerClient
from app.ingest import Ingest

logger = logging.getLogger("relay.webhooks")

router = APIRouter()

TopicHandler = Callable[[Ingest, str, bytes], None]

_TOPIC_SUFFIX_HANDLERS: dict[str, TopicHandler] = {
    "up": lambda ingest, topic, payload: ingest.handle_up(topic, payload),
    "status": lambda ingest, topic, payload: ingest.handle_status(topic, payload),
    "loc": lambda ingest, topic, payload: ingest.handle_loc(topic, payload),
}


def _topic_suffix(topic: str) -> str | None:
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "pager":
        return None
    return parts[2]


@router.post("/webhooks/mqtt")
async def mqtt_webhook(request: Request) -> Response:
    broker: BrokerClient = request.app.state.broker
    if not broker.verify_webhook(request):
        raise HTTPException(status_code=401, detail="invalid or missing webhook key")

    body = await request.body()
    parsed = broker.parse_webhook(body)
    if parsed is None:
        logger.warning("unparsable webhook body (%d bytes): %r", len(body), body[:200])
        return Response(status_code=200)

    topic, payload, _qos = parsed
    suffix = _topic_suffix(topic)
    handler = _TOPIC_SUFFIX_HANDLERS.get(suffix) if suffix else None
    if handler is None:
        logger.warning("webhook for unrecognised topic %s dropped", topic)
        return Response(status_code=200)

    ingest: Ingest = request.app.state.ingest
    try:
        # Off the event loop: handlers do blocking work -- sqlite writes and,
        # on an online edge (PROTOCOL.md §5.3), up to REPUBLISH_CAP
        # synchronous broker REST publishes at PUBLISH_TIMEOUT_S each. Run
        # inline, one slow /status webhook would stall every other request
        # this process is serving, including other webhooks.
        await run_in_threadpool(handler, ingest, topic, payload)
    except sqlite3.OperationalError:
        # A genuine relay-side storage failure (DB locked, disk full, ...),
        # not a malformed payload -- do NOT return 200, or we permanently
        # lose this message with no republish path (see module docstring).
        # Let it propagate to a 500 so the broker's rule engine retries.
        logger.exception("store error handling webhook for topic %s", topic)
        raise
    except Exception:
        # Never let an ingest bug turn into a non-2xx that makes the broker
        # retry-storm a message it will fail on again (§3.4's spirit,
        # extended to "our own bugs" as well as malformed device payloads).
        logger.exception("unexpected error handling webhook for topic %s", topic)
    return Response(status_code=200)

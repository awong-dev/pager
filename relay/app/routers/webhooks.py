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

A Firestore-layer failure (`google.api_core.exceptions.GoogleAPICallError` --
emulator/service unreachable, deadline exceeded, etc.) is also not a 200:
§3.4's 2xx-for-malformed-payload rule only covers payloads the relay will
never be able to make valid, not genuine relay-side storage failures.
Swallowing those would silently lose an up-message with no republish path
(§4.2 has no up-message republish rule), so we let it become a 500 instead
and rely on the broker's rule engine to retry the webhook.

`POST /webhooks/twilio/sms` and
`POST /webhooks/gchat`, docs/SERVER_PLAN.md §6.4/§6.5. Same
router-owns-auth-and-dispatch shape as `/webhooks/mqtt` above: each
endpoint validates its own provider-specific signature/token first (a
failure there is a 401, matching the webhook-key check's "auth failure, not
a malformed payload" carve-out) and then dispatches into
`app/backends/sms_twilio.py` / `app/backends/gchat.py` for the pure,
unit-tested verification logic, and `app/backends/resolve.py`'s
`resolve_reply()` for the shared `@alias`/single-peer recipient rule.

Two things the gchat handler does for security: it rejects linking a non-DM
space (`space.type != "DM"`) outright, and it pins the linking message's
`sender.name` in `gchatSpaces/{spaceId}` (`app/store/backends.py`'s
`set_gchat_space`) so every later message in that space is checked against
it before being treated as coming from the linked user -- without this, any
member of a linked space (not just its original 1:1 DM partner) could send
as the linked user. Separately, the "unlinked number" info log below
redacts to the last 4 digits (`_redact_phone`) rather than logging a full
E.164 phone number.

There is also a cheap **per-IP** rate limit on
`POST /webhooks/twilio/sms` and `POST /webhooks/gchat`, checked first, before
any signature/JWT verification or body parsing -- both endpoints are already
gated by a real signature/JWT check (Twilio's `X-Twilio-Signature`, Google's
Chat bearer JWT), so this is defense-in-depth against a flood of
forged-but-cheap-to-generate requests (constructing an invalid signature/JWT
costs an attacker nothing), not the primary control. Same
`app/store/rate_limits.py` fixed-window counter `POST /api/me/backends`/
`/api/admin/*` use, keyed `"webhook_ip:{sms|gchat}:{ip}"`. `RATE_LIMIT_
WEBHOOK_IP_LIMIT` calls per `RATE_LIMIT_WEBHOOK_IP_WINDOW_S` seconds
(defaults: 30 per minute) -- read fresh from the environment on every call,
same as this module's other per-call env lookups.
"""

from __future__ import annotations

import logging
import os
import time
from collections.abc import Callable

from fastapi import APIRouter, HTTPException, Request, Response
from google.api_core.exceptions import GoogleAPICallError
from starlette.concurrency import run_in_threadpool

from app.backends import gchat as gchat_backend
from app.backends import sms_twilio
from app.backends.resolve import USAGE_HINT, resolve_reply
from app.broker import BrokerClient
from app.ingest import Ingest
from app.notify import sms as sms_client
from app.routing import Routing
from app.store import backends as backends_store
from app.store import rate_limits as rate_limits_store

logger = logging.getLogger("relay.webhooks")

router = APIRouter()

DEFAULT_WEBHOOK_IP_LIMIT = 30
DEFAULT_WEBHOOK_IP_WINDOW_S = 60


def _webhook_ip_rate_limit() -> tuple[int, int]:
    limit = int(os.environ.get("RATE_LIMIT_WEBHOOK_IP_LIMIT", str(DEFAULT_WEBHOOK_IP_LIMIT)))
    window_s = int(
        os.environ.get("RATE_LIMIT_WEBHOOK_IP_WINDOW_S", str(DEFAULT_WEBHOOK_IP_WINDOW_S))
    )
    return limit, window_s


def _client_ip(request: Request) -> str:
    """The per-IP key must be the *original* caller's IP, not the TCP peer. Every real request to these two endpoints
    arrives Twilio/Google -> Firebase Hosting (`web/firebase.json`'s
    `/webhooks/**` rewrite) -> Cloud Run, so `request.client.host` is
    Google's own front-end address, identical for every caller: keying on it
    collapses all inbound SMS/Chat traffic into a single shared bucket, and
    an unauthenticated flooder could then 429 real Twilio/Chat deliveries
    for everyone (uvicorn only trusts `X-Forwarded-For` from
    `forwarded_allow_ips`, which defaults to 127.0.0.1 and does not match
    Cloud Run's proxy, so it never populates `request.client` from it).

    Google's front end *appends* to `X-Forwarded-For`, so element 0 is the
    originating client. A client can forge extra leading entries and thereby
    pick its own bucket -- accepted deliberately: this cap is explicit
    defense-in-depth *in front of* each endpoint's real signature/JWT check
    (see module docstring), so a bypass costs an attacker nothing they did
    not already have, while the shared-bucket alternative hands them a
    denial of service against legitimate traffic."""
    forwarded = request.headers.get("x-forwarded-for", "")
    if forwarded:
        first = forwarded.split(",")[0].strip()
        if first:
            return first
    return request.client.host if request.client is not None else "unknown"


def _check_webhook_ip_rate_limit(request: Request, bucket: str) -> None:
    ip = _client_ip(request)
    limit, window_s = _webhook_ip_rate_limit()
    if not rate_limits_store.check_and_increment(
        f"webhook_ip:{bucket}:{ip}", limit=limit, window_s=window_s
    ):
        raise HTTPException(status_code=429, detail="too many requests")

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


def _is_boot_ack_topic(topic: str) -> bool:
    """`pager/boot/{bid}/up` (docs/DEVICE_TASKS.md S2b.3, docs/PROTOCOL.md
    §2) -- one segment longer than the `pager/{device_id}/{suffix}` shape
    `_topic_suffix` matches, so it needs its own check rather than a
    `_TOPIC_SUFFIX_HANDLERS` entry."""
    parts = topic.split("/")
    return len(parts) == 4 and parts[0] == "pager" and parts[1] == "boot" and parts[3] == "up"


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
    handler: TopicHandler | None
    if _is_boot_ack_topic(topic):
        handler = lambda ingest, topic, payload: ingest.handle_boot_ack(topic, payload)
    else:
        suffix = _topic_suffix(topic)
        handler = _TOPIC_SUFFIX_HANDLERS.get(suffix) if suffix else None
    if handler is None:
        logger.warning("webhook for unrecognised topic %s dropped", topic)
        return Response(status_code=200)

    ingest: Ingest = request.app.state.ingest
    try:
        # Off the event loop: handlers do blocking work -- Firestore writes and,
        # on an online edge (PROTOCOL.md §5.3), up to REPUBLISH_CAP
        # synchronous broker REST publishes at PUBLISH_TIMEOUT_S each. Run
        # inline, one slow /status webhook would stall every other request
        # this process is serving, including other webhooks.
        await run_in_threadpool(handler, ingest, topic, payload)
    except GoogleAPICallError:
        # A genuine relay-side storage failure (Firestore unreachable,
        # deadline exceeded, ...), not a malformed payload -- do NOT return
        # 200, or we permanently lose this message with no republish path
        # (see module docstring). Let it propagate to a 500 so the broker's
        # rule engine retries.
        logger.exception("store error handling webhook for topic %s", topic)
        raise
    except Exception:
        # Never let an ingest bug turn into a non-2xx that makes the broker
        # retry-storm a message it will fail on again (§3.4's spirit,
        # extended to "our own bugs" as well as malformed device payloads).
        logger.exception("unexpected error handling webhook for topic %s", topic)
    return Response(status_code=200)


# ---------------------------------------------------------------------------
# POST /webhooks/twilio/sms -- docs/SERVER_PLAN.md §6.4
# ---------------------------------------------------------------------------


def _reject_hint(reason: str) -> str:
    return "unknown recipient" if reason == "unknown_alias" else "not allowed to message that recipient"


def _redact_phone(phone: str) -> str:
    """L4: last-4-digits only -- this is a log line for an *unrecognised*
    number (no user to attribute it to), so the full E.164 number has no
    operational value here and is PII best not left sitting in logs."""
    return f"...{phone[-4:]}" if len(phone) >= 4 else "..."


@router.post("/webhooks/twilio/sms")
async def twilio_sms_webhook(request: Request) -> Response:
    _check_webhook_ip_rate_limit(request, "sms")
    form = await request.form()
    # Twilio's params are single-valued (To/From/Body/...); last-value-wins
    # is a no-op for this webhook's real shape but keeps the type a plain
    # `dict[str, str]`, what `verify_twilio_signature` expects.
    params: dict[str, str] = {k: str(v) for k, v in form.items()}
    signature = request.headers.get("X-Twilio-Signature")
    url = sms_twilio.twilio_webhook_url()
    if not sms_twilio.verify_twilio_signature(url, params, signature, sms_client.auth_token()):
        raise HTTPException(status_code=401, detail="invalid Twilio signature")

    from_number = params.get("From")
    body = (params.get("Body") or "").strip()
    if not from_number:
        logger.warning("twilio webhook with no From field dropped")
        return Response(status_code=200)

    match = backends_store.get_by_phone(from_number)
    if match is None:
        # No verified user owns this number -- nothing to route to, and
        # texting an unrecognised number back would be both a cost and a
        # potential information leak. Log + drop, the same class of event
        # as `app/ingest.py`'s unregistered-device case.
        logger.info("sms webhook from unlinked number %s dropped", _redact_phone(from_number))
        return Response(status_code=200)
    uid, bid = match

    if len(body) > sms_twilio.SMS_BODY_MAX_CODEPOINTS:
        # §6.4: rejected with a usage hint, never truncated -- checked
        # here in the inbound-webhook handler, before any `routing.send()`
        # call, rather than in `deliver()`.
        sms_client.send_sms(from_number, sms_twilio.too_long_hint(len(body)))
        return Response(status_code=200)

    resolved = resolve_reply(uid, body)
    if resolved is None:
        sms_client.send_sms(from_number, USAGE_HINT)
        return Response(status_code=200)

    routing: Routing = request.app.state.routing
    result = routing.send(
        sender_uid=uid,
        recipient_alias=resolved.recipient_alias,
        kind="text",
        body=resolved.body,
        origin_backend_kind="sms",
        origin_backend_id=bid,
    )
    if result.rejected:
        sms_client.send_sms(from_number, _reject_hint(result.rejected[0].reason))
    return Response(status_code=200)


# ---------------------------------------------------------------------------
# POST /webhooks/gchat -- docs/SERVER_PLAN.md §6.5
# ---------------------------------------------------------------------------


def _chat_json(body: dict[str, str]) -> Response:
    import json

    return Response(
        content=json.dumps(body), status_code=200, media_type="application/json"
    )


@router.post("/webhooks/gchat")
async def gchat_webhook(request: Request) -> Response:
    _check_webhook_ip_rate_limit(request, "gchat")
    auth_header = request.headers.get("authorization", "")
    if not auth_header.lower().startswith("bearer "):
        raise HTTPException(status_code=401, detail="missing bearer token")
    # `partition`, not `split(None, 1)[1]`: a header of exactly `"Bearer "`
    # (trailing space, empty token) makes `split(None, 1)` return a
    # single-element list and `[1]` raise IndexError -> an unhandled 500
    # instead of a clean 401. See app/routers/internal.py's identical fix.
    token = auth_header.partition(" ")[2].strip()
    if not token:
        raise HTTPException(status_code=401, detail="empty bearer token")
    try:
        gchat_backend.verify_chat_bearer_token(token, audience=gchat_backend.gchat_audience())
    except Exception as exc:  # noqa: BLE001 -- verify_chat_bearer_token's own contract: any failure -> 401
        logger.warning("gchat webhook rejected invalid JWT: %r", exc)
        raise HTTPException(status_code=401, detail="invalid Chat token") from None

    try:
        payload = await request.json()
    except ValueError:
        return _chat_json({})
    if not isinstance(payload, dict):
        return _chat_json({})

    message = payload.get("message") or {}
    space_obj = message.get("space") or payload.get("space") or {}
    space = space_obj.get("name") if isinstance(space_obj, dict) else None
    # M2: Chat's space `type` ("DM" for a 1:1, "ROOM"/"SPACE" for a group) --
    # §6.5 assumes the link is a 1:1 DM; nothing else in this handler
    # enforces that, so a group space must never be allowed to link at all.
    space_type = space_obj.get("type") if isinstance(space_obj, dict) else None
    sender_obj = message.get("sender") if isinstance(message.get("sender"), dict) else {}
    sender_name = sender_obj.get("name")
    text = (message.get("text") or "").strip()
    if not space:
        return _chat_json({})
    space_id = space.rsplit("/", 1)[-1]

    if text.lower().startswith("/link"):
        parts = text.split(None, 1)
        code = parts[1].strip() if len(parts) == 2 else ""
        popped = backends_store.pop_gchat_link_code(code) if code else None
        if popped is None:
            return _chat_json({"text": "That code is invalid or already used."})
        uid, bid, expires_at = popped
        if time.time() > expires_at:
            return _chat_json(
                {"text": "That code expired -- get a new one from Settings > Backends."}
            )
        if space_type != "DM":
            # M2: reject linking a non-DM space outright -- a group space
            # would let every member of it send as the linked user (§6.5's
            # unstated but assumed 1:1 invariant).
            logger.warning(
                "SECURITY gchat link rejected: space %s is not a DM (type=%r)", space_id, space_type
            )
            return _chat_json(
                {"text": "Pager can only be linked from a direct message, not a group space."}
            )
        row = backends_store.get_backend(uid, bid)
        config = dict(row.config) if row is not None else {}
        config.pop("linkCode", None)
        config.pop("linkCodeExpiresAt", None)
        config["space"] = space
        backends_store.update_backend(uid, bid, config=config, verified=True, enabled=True)
        # M2: pin the linking message's sender -- every later message in
        # this space is checked against it below.
        backends_store.set_gchat_space(space_id, uid, bid, sender_name)
        return _chat_json({"text": "Linked! You can now send messages from here."})

    match = backends_store.get_by_gchat_space(space_id)
    if match is None:
        return _chat_json(
            {"text": "This space isn't linked yet -- send /link <code> from Settings > Backends."}
        )
    uid, bid, linked_sender = match
    if linked_sender is not None and sender_name != linked_sender:
        # A different member of the (supposedly 1:1) space sent this --
        # never attribute it to the linked user. `linked_sender is None`
        # means a space linked before senders were pinned; permissive there
        # rather than breaking it.
        logger.warning(
            "SECURITY gchat message from sender %r in space %s does not match linked sender %r "
            "(bid=%s) -- ignored",
            sender_name,
            space_id,
            linked_sender,
            bid,
        )
        return _chat_json({})

    resolved = resolve_reply(uid, text)
    if resolved is None:
        return _chat_json({"text": USAGE_HINT})

    routing: Routing = request.app.state.routing
    result = routing.send(
        sender_uid=uid,
        recipient_alias=resolved.recipient_alias,
        kind="text",
        body=resolved.body,
        origin_backend_kind="gchat",
        origin_backend_id=bid,
    )
    if result.rejected:
        return _chat_json({"text": _reject_hint(result.rejected[0].reason)})
    return _chat_json({})

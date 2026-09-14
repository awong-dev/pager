"""`sms` backend -- the Twilio adapter, docs/SERVER_PLAN.md §6.4.

- **Outbound** (`deliver()`): `app/notify/sms.py`'s `send_sms()`, `From` =
  the deployment's one Twilio number, `To` = `backend.config["phone"]`.
  Points at `TWILIO_BASE_URL` (unset -> the delivery stays `queued`, a
  recoverable state the tick retry picks up), so this runs unmodified
  against
  `tools/mocks/twilio_mock.py` in dev/test and the real Twilio API in prod.
- **Link flow**: `start_link()` sends a 6-digit code by SMS and stashes a
  *hash* of it (plus its expiry) in the server-only `smsVerifyCodes/{bid}`
  collection (`app/store/backends.py`'s `set_sms_verify_code` -- see that
  module's docstring for why this is **not** `backend.config`, H1: `config`
  is readable by the backend's own owner via `firestore.rules`, i.e. by
  exactly the person a phone claim needs to be verified against, which would
  let anyone read the code straight out of Firestore instead of receiving
  the SMS). `complete_link()` is a pure proof-check against that stashed
  hash (see `backends/base.py`'s docstring -- neither `start_link` nor
  `complete_link` know the owning `uid`, only the `BackendRow`, so the
  actual `verifiedAt`/`phoneIndex` writes on success are the caller's job,
  `app/routers/me.py`'s `POST /api/me/backends/{id}/verify`).
- **Inbound webhook** (`POST /webhooks/twilio/sms`, `app/routers/
  webhooks.py`): signature verification and request dispatch live there
  (matching `POST /webhooks/mqtt`'s existing router-owns-auth-and-dispatch
  shape); `verify_twilio_signature()` here is the pure, unit-testable
  HMAC-SHA1 check (docs/SERVER_PLAN.md §6.4, Twilio's documented algorithm:
  https://www.twilio.com/docs/usage/webhooks/webhooks-security -- base64(
  HMAC-SHA1(authToken, url + every POST param's key+value, sorted by key,
  concatenated with no separator))). `@alias`/single-peer resolution is
  `app/backends/resolve.py`'s `resolve_reply()`, shared with `gchat.py`.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import logging
import os
import re
import time

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.notify import sms as sms_client
from app.store import backends as backends_store
from app.store import messages as messages_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User
from app.wire import BODY_MAX_CODEPOINTS

logger = logging.getLogger("relay.backends.sms_twilio")

LOC_PREVIEW = "location"
# docs/SERVER_PLAN.md §6.4: a code sent by `start_link`, checked by
# `complete_link` -- 6 digits, the same shape Firebase's own phone-auth
# codes use, expiring in 10 minutes (arbitrary but generous: a link flow is
# a one-off setup action, not a hot path).
VERIFY_CODE_TTL_S = 600


class SmsConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    phone: str


def _render_body(msg: Message) -> str:
    """Outbound rendering. `kind='loc'` (a location answer, §5.6) has no
    `body`, only `loc`; a short fixed preview stands in for a real
    "lat,lon" rendering."""
    if msg.body:
        return msg.body
    if msg.kind == "loc" and msg.loc is not None:
        return LOC_PREVIEW
    return "(no content)"


def _new_code() -> str:
    import secrets

    return f"{secrets.randbelow(1_000_000):06d}"


def _hash_code(code: str) -> str:
    """One-way hash of a verify code before it is stored in `smsVerifyCodes`
    (H1) -- see `app/store/backends.py`'s module docstring for why hashing
    is defense in depth, not the real control (the real control is
    `smsVerifyCodes` having no `firestore.rules` `match` block at all)."""
    return hashlib.sha256(code.encode("utf-8")).hexdigest()


# M1: `phoneIndex` document ids (and `smsVerifyCodes`-adjacent phone use)
# must be normalised E.164 -- Twilio's `From` field is always E.164, but a
# user-typed `config.phone` is not guaranteed to be, and a raw string
# containing `/` is not a legal Firestore document id (would raise on
# `.document(phone)` instead of failing with a clean validation error).
_E164_RE = re.compile(r"^\+[1-9]\d{6,14}$")


def normalize_e164(phone: str) -> str:
    """Strips everything but digits and a leading `+`, then requires the
    result to actually look like E.164 (`+` followed by 7-15 digits, first
    digit 1-9) -- raises `ValueError` (caught by the caller and turned into
    a 400) rather than silently accepting/mangling an invalid number."""
    digits = "".join(ch for ch in phone if ch.isdigit())
    normalized = f"+{digits}"
    if not _E164_RE.match(normalized):
        raise ValueError(f"not a valid E.164 phone number: {phone!r}")
    return normalized


class SmsTwilioBackend:
    kind = "sms"
    config_schema = SmsConfig

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        phone = backend.config.get("phone")
        if not phone:
            logger.warning("sms backend %s has no phone configured", backend.id)
            return DeliverResult(ok=False, state="failed", error="sms backend missing phone")

        body = _render_body(msg)
        result = sms_client.send_sms(phone, body)
        if result.ok:
            messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
            return DeliverResult(ok=True, state="sent", external_id=result.sid)
        # TWILIO_BASE_URL unconfigured and an actual send failure are both
        # "try again later", not "give up now" -- `record_delivery_attempt`
        # (app/routing.py) is what eventually gives up after
        # MAX_DELIVERY_ATTEMPTS.
        return DeliverResult(ok=False, state="queued", error=result.error)

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        phone = backend.config.get("phone")
        if not phone:
            return None
        code = _new_code()
        expires_at = int(time.time()) + VERIFY_CODE_TTL_S
        # H1: hashed, and in the server-only `smsVerifyCodes` collection --
        # NOT `backend.config` (owner-readable, see this module's and
        # `app/store/backends.py`'s docstrings for why that defeats
        # verification entirely).
        backends_store.set_sms_verify_code(backend.id, _hash_code(code), expires_at)
        sms_client.send_sms(phone, f"Your pager verification code is {code}")
        return LinkStep(
            instructions="Enter the code we texted you.", expiresInS=VERIFY_CODE_TTL_S
        )

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        record = backends_store.get_sms_verify_code(backend.id)
        if record is None:
            return False
        code_hash, expires_at = record
        if time.time() > expires_at:
            return False
        # Bytes, not `str` -- `proof` is caller-supplied JSON and
        # `hmac.compare_digest` raises `TypeError` on non-ASCII `str`
        # (which would turn a wrong code into a 500 instead of a 400).
        # Hashes are compared, not raw codes (H1) -- both sides are hex
        # digests (ASCII), so this also sidesteps the non-ASCII `TypeError`
        # `proof` itself could otherwise trigger.
        return hmac.compare_digest(
            code_hash.encode("utf-8"), _hash_code(proof.strip()).encode("utf-8")
        )

    def render_state(self, delivery: Delivery) -> str:
        return delivery.state


# ---------------------------------------------------------------------------
# inbound webhook support -- X-Twilio-Signature verification and the
# 160-code-point pre-`routing.send()` body limit (both exercised for real
# by `app/routers/webhooks.py`'s `/webhooks/twilio/sms`).
# ---------------------------------------------------------------------------

SMS_BODY_MAX_CODEPOINTS = BODY_MAX_CODEPOINTS  # 160 -- docs/SERVER_PLAN.md §6.4


def too_long_hint(n: int) -> str:
    return (
        f"Message too long ({n} chars, max {SMS_BODY_MAX_CODEPOINTS}) -- not sent. "
        "Shorten it and try again."
    )


def compute_twilio_signature(url: str, params: dict[str, str], auth_token: str) -> str:
    """Twilio's documented algorithm: HMAC-SHA1(authToken, url + every POST
    param's key immediately followed by its value, sorted by key, no
    separators between pairs), base64-encoded."""
    data = url + "".join(f"{key}{params[key]}" for key in sorted(params))
    digest = hmac.new(auth_token.encode("utf-8"), data.encode("utf-8"), hashlib.sha1).digest()
    return base64.b64encode(digest).decode("utf-8")


def verify_twilio_signature(
    url: str, params: dict[str, str], signature: str | None, auth_token: str
) -> bool:
    """Fails closed: a blank `auth_token` (unconfigured deployment) or a
    missing header never verifies, matching `BrokerClient.verify_webhook`'s
    "unconfigured is not open" rule."""
    if not auth_token or not signature:
        return False
    expected = compute_twilio_signature(url, params, auth_token)
    # `.encode()` both sides: `hmac.compare_digest` raises `TypeError` on a
    # non-ASCII `str` argument, and `X-Twilio-Signature` is attacker-
    # controlled header text (Starlette decodes headers as latin-1, so a
    # single 0xFF byte produces a non-ASCII `str`). Comparing bytes keeps a
    # forged header a 401 rather than an unhandled 500.
    return hmac.compare_digest(expected.encode("utf-8"), signature.encode("utf-8"))


def twilio_webhook_url() -> str:
    """The exact URL Twilio was configured to POST to -- must match
    byte-for-byte what the Twilio console has on file for the signature to
    verify, so this is read from `PUBLIC_BASE_URL` (the deployment's public
    HTTPS origin; unset in dev, where this whole endpoint has no working
    signature check against a real Twilio account anyway) rather than
    reconstructed from the inbound request (which can't be trusted -- a
    reverse proxy can rewrite scheme/host, and Twilio signs the URL *it*
    called, not whatever a request object might report)."""
    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    return f"{base}/webhooks/twilio/sms"


# Re-exported for convenience so `app/routers/webhooks.py` need import only
# this module for everything sms-webhook-related.
send_sms = sms_client.send_sms

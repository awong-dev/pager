"""`sms` backend -- the Twilio adapter, docs/RELAY_SMS_DESIGN.md decisions 3-4.

- **Outbound** (`deliver()`): `app/notify/sms.py`'s `send_sms()`. The row it
  runs for lives on an *external* (an SMS contact); `To` = `backend.config
  ["phone"]`, `From` = the **sender's** `users.smsNumber`. A sender without a
  number fails at once (`no_sms_number`, never retried). Twilio 4xx (invalid
  or opted-out number, filtered, ...) -> `failed`; 5xx / transport -> `queued`
  (the tick retries, `record_delivery_attempt` ends it). No link flow: the
  admin asserts the number.
- **Inbound** webhook (`POST /webhooks/twilio/sms`, `app/routers/
  webhooks.py`): signature verification and dispatch live there;
  `verify_twilio_signature()` here is the pure HMAC-SHA1 check (Twilio's
  documented algorithm: https://www.twilio.com/docs/usage/webhooks/
  webhooks-security -- base64(HMAC-SHA1(authToken, url + every POST param's
  key+value, sorted by key, concatenated with no separator))).
- **Consent** (decision 11): outbound goes only to a number with an
  `smsConsent` row `opted_in` (else `failed`, `not_opted_in` / `opted_out`,
  never retried). The body is `<Name> says: "<defanged text>" - Pager
  (<operator>)`, plus the STOP/HELP disclosure on the first relayed message to
  a number per UTC day. The day's disclosure is claimed *before* the send; if
  the send then fails the claim stands, so a retry the same day goes out
  without it.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import logging
import os
import re
from datetime import UTC, datetime

from pydantic import BaseModel, ConfigDict

from app import sms_compliance
from app.backends.base import DeliverResult, LinkStep
from app.notify import sms as sms_client
from app.store import messages as messages_store
from app.store import sms_consent
from app.store import users as users_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User
from app.wire import BODY_MAX_CODEPOINTS

logger = logging.getLogger("relay.backends.sms_twilio")

LOC_PREVIEW = "location"


class SmsConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    phone: str


def _today_utc() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%d")


def _render_body(msg: Message) -> str:
    """Outbound rendering. `kind='loc'` has no `body`, only `loc`; a short
    fixed preview stands in for a real "lat,lon" rendering."""
    if msg.body:
        return msg.body
    if msg.kind == "loc" and msg.loc is not None:
        return LOC_PREVIEW
    return "(no content)"


class SmsTwilioBackend:
    kind = "sms"
    config_schema = SmsConfig

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        phone = backend.config.get("phone")
        if not phone:
            logger.warning("sms backend %s has no phone configured", backend.id)
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id)
            return DeliverResult(ok=False, state="failed", error="sms backend missing phone")

        sender = users_store.get_user(msg.senderUid)
        from_number = sender.smsNumber if sender is not None else None
        if not from_number:
            logger.info(
                "sms out to=%s from=- sid=- status=failed code=no_sms_number",
                sms_client.redact_phone(phone),
            )
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id)
            return DeliverResult(ok=False, state="failed", error="no_sms_number")

        consent = sms_consent.get(phone)
        if consent is None or consent.status != "opted_in":
            code = "opted_out" if consent is not None else "not_opted_in"
            logger.info(
                "sms out to=%s from=%s sid=- status=failed code=%s",
                sms_client.redact_phone(phone),
                sms_client.redact_phone(from_number),
                code,
            )
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id)
            return DeliverResult(ok=False, state="failed", error=code)

        text = sms_compliance.relay_body(
            sender.displayName if sender is not None else "",
            sms_compliance.defang(_render_body(msg)),
        )
        if sms_consent.claim_disclosure(phone, _today_utc()):
            text += sms_compliance.DISCLOSURE
        result = sms_client.send_sms(phone, text, from_number=from_number)
        status = "sent" if result.ok else ("queued" if result.transient else "failed")
        logger.info(
            "sms out to=%s from=%s sid=%s status=%s code=%s",
            sms_client.redact_phone(phone),
            sms_client.redact_phone(from_number),
            result.sid or "-",
            status,
            result.code if result.code is not None else "-",
        )
        if result.ok:
            messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
            return DeliverResult(ok=True, state="sent", external_id=result.sid)
        if status == "failed":
            messages_store.mark_delivery_failed_if_queued(msg.id, backend.id)
        return DeliverResult(ok=False, state=status, error=result.error)  # type: ignore[arg-type]

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return False

    def render_state(self, delivery: Delivery) -> str:
        return "sent by SMS" if delivery.state == "sent" else delivery.state


# ---------------------------------------------------------------------------
# inbound webhook support -- X-Twilio-Signature verification and the
# 160-code-point pre-`routing.send()` body limit.
# ---------------------------------------------------------------------------

SMS_BODY_MAX_CODEPOINTS = BODY_MAX_CODEPOINTS  # 160
SMS_BODY_MAX_UTF8_BYTES = 320  # docs/PROTOCOL.md §3.1

_CONTROL_RE = re.compile("[\u0000-\u001f\u007f]")


def pager_body(raw: str) -> str:
    """docs/PROTOCOL.md §3.1: each control character (U+0000-U+001F, U+007F)
    becomes a space, nothing else is collapsed, then strip."""
    return _CONTROL_RE.sub(" ", raw).strip()


def body_too_long(body: str) -> bool:
    """More than 160 code points or 320 UTF-8 bytes (§3.1)."""
    return len(body) > SMS_BODY_MAX_CODEPOINTS or len(body.encode("utf-8")) > SMS_BODY_MAX_UTF8_BYTES


def too_long_hint(_n: int | None = None) -> str:
    return f"Message too long (max {SMS_BODY_MAX_CODEPOINTS} characters) -- not sent."


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
    missing header never verifies."""
    if not auth_token or not signature:
        return False
    expected = compute_twilio_signature(url, params, auth_token)
    # Bytes, not `str`: `hmac.compare_digest` raises `TypeError` on a
    # non-ASCII `str`, and the header is attacker-controlled (Starlette
    # decodes headers as latin-1), so a forged header stays a 401, not a 500.
    return hmac.compare_digest(expected.encode("utf-8"), signature.encode("utf-8"))


def twilio_webhook_url() -> str:
    """The exact URL Twilio was configured to POST to -- must match
    byte-for-byte what the Twilio console has on file for the signature to
    verify, so it is read from `PUBLIC_BASE_URL` (the deployment's public
    HTTPS origin) rather than reconstructed from the inbound request (a
    reverse proxy can rewrite scheme/host; Twilio signs the URL *it* called)."""
    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    return f"{base}/webhooks/twilio/sms"

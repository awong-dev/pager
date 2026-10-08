"""Twilio Messages API client -- docs/RELAY_SMS_DESIGN.md decisions 3 and 9.

Owns the *one* HTTP call this deployment makes to Twilio (or, in dev/test, to
`tools/mocks/twilio_mock.py` via `TWILIO_BASE_URL`): `app/backends/
sms_twilio.py`'s `deliver()` goes through `send_sms()`. `From` is always the
sending *person's* own number (`users.smsNumber`), passed explicitly by the
caller; there is no deployment-wide default number.

Credentials and endpoint are read fresh from the environment on every call:
`TWILIO_BASE_URL` (unset -> the send is a transient failure, so the delivery
stays `queued`), `TWILIO_ACCOUNT_SID`, `TWILIO_AUTH_TOKEN` (HTTP Basic auth
against Twilio, and the HMAC-SHA1 key for the inbound `X-Twilio-Signature`
check in `app/backends/sms_twilio.py`). Neither is ever logged.
"""

from __future__ import annotations

import logging
import os

import httpx
from pydantic import BaseModel, ConfigDict

logger = logging.getLogger("relay.notify.sms")

REQUEST_TIMEOUT_S = 5.0
DEFAULT_ACCOUNT_SID = "ACdev0000000000000000000000000000"


def redact_phone(phone: str) -> str:
    """Last-4-digits only: the full number is PII nothing downstream needs."""
    return f"...{phone[-4:]}" if len(phone) >= 4 else "..."


class TwilioSendResult(BaseModel):
    model_config = ConfigDict(extra="ignore")

    ok: bool
    sid: str | None = None
    error: str | None = None
    # Twilio's own error code (21211, 21610, ...) when the API gave one.
    code: int | None = None
    # True when a retry may succeed (transport error, 5xx, 429, unconfigured);
    # False for a definitive 4xx.
    transient: bool = False


def base_url() -> str | None:
    return os.environ.get("TWILIO_BASE_URL") or None


def account_sid() -> str:
    return os.environ.get("TWILIO_ACCOUNT_SID", DEFAULT_ACCOUNT_SID)


def auth_token() -> str:
    """Empty in dev/test against the mock; the signature check treats an
    empty token as "never verifies" (fail closed)."""
    return os.environ.get("TWILIO_AUTH_TOKEN", "")


def send_sms(to: str, body: str, *, from_number: str) -> TwilioSendResult:
    """`POST .../Accounts/{Sid}/Messages.json`, the exact Twilio REST shape
    `tools/mocks/twilio_mock.py` mirrors. Never raises: a transport error or a
    non-2xx comes back as `TwilioSendResult(ok=False, ...)`."""
    url = base_url()
    if url is None:
        return TwilioSendResult(ok=False, error="TWILIO_BASE_URL not configured", transient=True)
    sid = account_sid()
    try:
        resp = httpx.post(
            f"{url.rstrip('/')}/2010-04-01/Accounts/{sid}/Messages.json",
            data={"To": to, "From": from_number, "Body": body},
            auth=(sid, auth_token()),
            timeout=REQUEST_TIMEOUT_S,
        )
    except httpx.HTTPError as exc:
        logger.warning("twilio send failed (to=%s): %r", redact_phone(to), exc)
        return TwilioSendResult(ok=False, error=f"sms request failed: {exc}", transient=True)

    if 200 <= resp.status_code < 300:
        sid_out = None
        try:
            sid_out = resp.json().get("sid")
        except ValueError:
            pass
        return TwilioSendResult(ok=True, sid=sid_out)

    code: int | None = None
    try:
        raw = resp.json().get("code")
        code = int(raw) if raw is not None else None
    except (ValueError, TypeError, AttributeError):
        pass
    transient = resp.status_code >= 500 or resp.status_code == 429
    logger.warning(
        "twilio send rejected (to=%s status=%s code=%s)",
        redact_phone(to),
        resp.status_code,
        code,
    )
    return TwilioSendResult(
        ok=False,
        error=f"twilio returned {resp.status_code}" + (f" code {code}" if code else ""),
        code=code,
        transient=transient,
    )

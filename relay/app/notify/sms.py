"""Twilio Messages API client -- docs/SERVER_PLAN.md §5's `notify/` package
line ("notify/sms.py (Twilio) -- used by the sms backend and its link
flow"). Owns the *one* HTTP call this deployment ever makes to Twilio (or,
in dev/test, to `tools/mocks/twilio_mock.py` via `TWILIO_BASE_URL`) --
`app/backends/sms_twilio.py`'s `deliver()` (a relay-originated down
message) and its `start_link()` (a phone-verification code SMS) both go
through `send_sms()` so there is exactly one place that builds the
Messages-API request, matching `tools/mocks/twilio_mock.py`'s module
docstring ("a real `sms_twilio.py` adapter ... needs only to override the
SDK's base URL to point at this mock").

Every credential/endpoint here is read fresh from the environment on every
call (`_base_url()`/`_account_sid()`/... -- the same per-call
`os.environ.get(...)` pattern `app/location.py`'s `loc_req_ttl_s()` uses),
not threaded through `app.config.Settings`. The surface is
`TWILIO_BASE_URL`/`TWILIO_ACCOUNT_SID`/`TWILIO_FROM_NUMBER`, plus
`TWILIO_AUTH_TOKEN` for real HTTP Basic auth against Twilio (the mock
ignores it, as it does the account SID) and for
`app/backends/sms_twilio.py`'s inbound `X-Twilio-Signature` check.
"""

from __future__ import annotations

import logging
import os

import httpx
from pydantic import BaseModel, ConfigDict

logger = logging.getLogger("relay.notify.sms")

REQUEST_TIMEOUT_S = 5.0
DEFAULT_ACCOUNT_SID = "ACdev0000000000000000000000000000"
DEFAULT_FROM_NUMBER = "+15005550006"  # Twilio's own "always valid" magic test number


def _redact_phone(phone: str) -> str:
    """Last-4-digits only, the same style `app/routers/webhooks.py` uses
    -- `to` here is a real, attributed phone number (unlike that module's
    "unrecognised number" case), which
    makes logging it in full even less justified: nothing below needs the
    full number to be actionable, and it is PII best not left sitting in
    plaintext logs."""
    return f"...{phone[-4:]}" if len(phone) >= 4 else "..."


class TwilioSendResult(BaseModel):
    model_config = ConfigDict(extra="ignore")

    ok: bool
    sid: str | None = None
    error: str | None = None


def base_url() -> str | None:
    """`None` means "this deployment has no sms backend wired up yet" -- the
    same recoverable-not-fatal shape `app/backends/pager.py`'s missing
    `deviceId` case uses."""
    return os.environ.get("TWILIO_BASE_URL") or None


def account_sid() -> str:
    return os.environ.get("TWILIO_ACCOUNT_SID", DEFAULT_ACCOUNT_SID)


def auth_token() -> str:
    """Twilio's REST auth token -- also the HMAC-SHA1 key for
    `X-Twilio-Signature` verification (`app/backends/sms_twilio.py`). Empty
    string in dev/test against the mock, which authenticates nothing;
    `sms_twilio.py`'s signature check treats an empty token as "never
    verifies" (fail closed) rather than "always verifies"."""
    return os.environ.get("TWILIO_AUTH_TOKEN", "")


def from_number() -> str:
    return os.environ.get("TWILIO_FROM_NUMBER", DEFAULT_FROM_NUMBER)


def send_sms(to: str, body: str) -> TwilioSendResult:
    """`POST .../Accounts/{Sid}/Messages.json`, the exact Twilio REST shape
    `tools/mocks/twilio_mock.py` mirrors. Never raises -- a transport error
    or a non-2xx both come back as `TwilioSendResult(ok=False, error=...)`,
    a "leave it queued/failed, let the caller decide" contract."""
    url = base_url()
    if url is None:
        return TwilioSendResult(ok=False, error="TWILIO_BASE_URL not configured")
    sid = account_sid()
    try:
        resp = httpx.post(
            f"{url.rstrip('/')}/2010-04-01/Accounts/{sid}/Messages.json",
            data={"To": to, "From": from_number(), "Body": body},
            auth=(sid, auth_token()),
            timeout=REQUEST_TIMEOUT_S,
        )
    except httpx.HTTPError as exc:
        logger.warning("twilio send failed (to=%s): %r", _redact_phone(to), exc)
        return TwilioSendResult(ok=False, error=f"sms request failed: {exc}")

    if 200 <= resp.status_code < 300:
        sid_out = None
        try:
            sid_out = resp.json().get("sid")
        except ValueError:
            pass
        return TwilioSendResult(ok=True, sid=sid_out)

    logger.warning(
        "twilio send rejected (to=%s status=%s body=%r)",
        _redact_phone(to),
        resp.status_code,
        resp.text[:200],
    )
    return TwilioSendResult(ok=False, error=f"twilio returned {resp.status_code}")

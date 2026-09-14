"""`sms` backend -- **Phase 5 stub**, not the real Twilio adapter
docs/SERVER_PLAN.md §6.4 describes (Twilio signature verification, the
`start_link`/`complete_link` phone-verify flow, inbound webhook resolution,
US A2P 10DLC concerns). This phase only needs enough of an `sms` backend
implementing `backends/base.py`'s `Backend` protocol to (a) call a Twilio-
Messages-API-shaped mock (`tools/mocks/twilio_mock.py`) via a `TWILIO_BASE_URL`
override and (b) mark the delivery `'sent'` on success / leave it `'queued'`
on failure, so `app/jobs.py`'s `tick()` retry mechanics (extended this phase
to cover non-pager backends too, see that module) have a real second backend
kind to exercise end to end (`tools/e2e_v2.py`'s `fanout` scenario).

TODO(orchestrator): replace with the real Twilio adapter in Phase 7
(signature verification, link flow, 10DLC).

Config: `SmsConfig.phone` (E.164-ish string, not validated beyond "non-empty"
this phase -- real phone validation is Phase 7's `start_link`/`complete_link`
verify flow). `TWILIO_BASE_URL` (no default -- unconfigured means "this
deployment has no sms backend wired up yet", the same "leave it queued,
don't crash" shape `PagerBackend.deliver()` uses for a backend with no
`deviceId`) is read fresh from the environment on every `deliver()` call,
the same `os.environ.get(...)`-per-call pattern `app/location.py`'s
`loc_req_ttl_s()` uses, so tests can flip it between calls without reloading
this module. `TWILIO_ACCOUNT_SID`/`TWILIO_FROM_NUMBER` are similarly
env-read stand-ins for what a real per-deployment Twilio account/number
would be (§6.4: "the deployment's one Twilio number") -- meaningless to the
mock, which ignores auth and echoes back whatever `From` it's given, but
kept as real-shaped request fields so Phase 7's adapter changes nothing
about how this module builds its request, only how it authenticates.
"""

from __future__ import annotations

import logging
import os

import httpx
from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.store import messages as messages_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.sms_stub")

REQUEST_TIMEOUT_S = 5.0
DEFAULT_ACCOUNT_SID = "ACdev0000000000000000000000000000"
DEFAULT_FROM_NUMBER = "+15005550006"  # Twilio's own "always valid" magic test number
LOC_PREVIEW = "location"


class SmsConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    phone: str


def _base_url() -> str | None:
    return os.environ.get("TWILIO_BASE_URL") or None


def _account_sid() -> str:
    return os.environ.get("TWILIO_ACCOUNT_SID", DEFAULT_ACCOUNT_SID)


def _from_number() -> str:
    return os.environ.get("TWILIO_FROM_NUMBER", DEFAULT_FROM_NUMBER)


def _render_body(msg: Message) -> str:
    """This stub only needs a plausible message body, not the real
    `sms_twilio.py` adapter's 160-code-point pre-`routing.send()` truncation
    rule (§6.4) -- that rule guards an *inbound* SMS reply's length before
    it is turned into a `routing.send()` call, which is unrelated to what
    this module renders for an *outbound* send. `kind='loc'` (a location
    answer, §5.6) has no `body`, only `loc`; a short fixed preview stands in
    for the real adapter's eventual "lat,lon" rendering."""
    if msg.body:
        return msg.body
    if msg.kind == "loc" and msg.loc is not None:
        return LOC_PREVIEW
    return "(no content)"


class SmsStubBackend:
    kind = "sms"
    config_schema = SmsConfig

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        phone = backend.config.get("phone")
        if not phone:
            logger.warning("sms backend %s has no phone configured", backend.id)
            return DeliverResult(ok=False, state="failed", error="sms backend missing phone")

        base_url = _base_url()
        if base_url is None:
            # Same shape as PagerBackend's "no deviceId configured" case,
            # but recoverable (a later tick retries once TWILIO_BASE_URL is
            # set) rather than an immediate 'failed' -- there's nothing
            # actually wrong with this *delivery*, only with this
            # deployment's current configuration.
            return DeliverResult(ok=False, state="queued", error="TWILIO_BASE_URL not configured")

        body = _render_body(msg)
        try:
            resp = httpx.post(
                f"{base_url.rstrip('/')}/2010-04-01/Accounts/{_account_sid()}/Messages.json",
                data={"To": phone, "From": _from_number(), "Body": body},
                timeout=REQUEST_TIMEOUT_S,
            )
        except httpx.HTTPError as exc:
            logger.warning("sms mock request failed (msg=%s): %r", msg.id, exc)
            return DeliverResult(ok=False, state="queued", error=f"sms request failed: {exc}")

        if 200 <= resp.status_code < 300:
            messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
            sid = None
            try:
                sid = resp.json().get("sid")
            except ValueError:
                pass
            return DeliverResult(ok=True, state="sent", external_id=sid)

        logger.warning(
            "sms mock rejected send (msg=%s status=%s body=%r)", msg.id, resp.status_code, resp.text[:200]
        )
        return DeliverResult(ok=False, state="queued", error=f"sms mock returned {resp.status_code}")

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        # Real phone verification (send a code, confirm it) is Phase 7's
        # `start_link`/`complete_link` -- this stub's config is set directly
        # (`POST /api/me/backends {"kind": "sms", "config": {"phone": ...}}`),
        # unverified, which is fine for exercising fan-out/retry this phase.
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return False

    def render_state(self, delivery: Delivery) -> str:
        return delivery.state

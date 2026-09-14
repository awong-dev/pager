"""`gchat` backend -- Google Chat app, docs/SERVER_PLAN.md §6.5.

- **Outbound** (`deliver()`): `spaces.messages.create`
  (`POST https://chat.googleapis.com/v1/{space}/messages`) using the
  relay's own ADC service-account credentials -- no shared secret. In dev/
  test, where no real GCP service account exists, `ChatClient` is a small
  injectable Protocol (`NullChatClient` by default), the exact pattern
  `app/backends/webapp.py`'s `FCMClient`/`NullFCMClient` already established
  for "a real credential-requiring Google API client that must still be
  testable locally".
- **Link flow is inbound-message-driven, not outbound-code-driven**
  (§6.5): `start_link()` still fits the `Backend` protocol's "e.g. send a
  code" shape, but the code is *shown to the user on the web app*
  (`/settings/backends`) rather than sent anywhere -- the user
  types it back at the app themselves, as `/link <code>` in a Chat DM.
  `complete_link()` is therefore never the real completion path (it always
  returns `False`; kept only for `Backend`-protocol conformance) -- the
  *inbound* webhook (`app/routers/webhooks.py`'s `/webhooks/gchat`) is what
  actually matches the code (via `app/store/backends.py`'s
  `pop_gchat_link_code`) and marks the backend verified, because Chat's
  event carries the DM `space` resource name the backend config needs to
  store, and only the inbound path ever sees that.
- **Inbound webhook**: Google-issued JWT verification lives here
  (`verify_chat_bearer_token`) as a pure, unit-testable function; dispatch
  (reading the event body, `/link` vs. ordinary message, calling
  `routing.send()`) lives in `app/routers/webhooks.py`, matching how
  `sms_twilio.py`/`app/routers/webhooks.py` split the same concerns for
  Twilio. `@alias`/single-peer resolution is `app/backends/resolve.py`'s
  `resolve_reply()`, shared with `sms_twilio.py`.

**Unverified prerequisite**: Chat apps are documented to require the
installing Google account to be on Google Workspace, not consumer Gmail.
That has not been confirmed against a real Workspace console. The adapter is
built on the documented contract regardless; if the restriction does hold
for a given deployment, Email (§6.6) is the fallback slot, not built here.
"""

from __future__ import annotations

import logging
import os
import secrets
import time
from collections.abc import Mapping
from typing import Any, Protocol

import google.auth as google_auth
import google.auth.jwt as google_jwt
import google.auth.transport.requests as google_requests
import google.oauth2.id_token as google_id_token
from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.store import backends as backends_store
from app.store import messages as messages_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.gchat")

CHAT_API_BASE_URL = "https://chat.googleapis.com/v1"
# Google Chat's documented cert endpoint for verifying its own bearer
# tokens (best-effort citation -- see this module's docstring's caveat
# about what could not be verified in this sandbox):
# https://developers.google.com/workspace/chat/authenticate-authorize-chat-app
DEFAULT_CERTS_URL = (
    "https://www.googleapis.com/service_accounts/v1/metadata/x509/"
    "chat@system.gserviceaccount.com"
)
CHAT_ISSUER = "chat@system.gserviceaccount.com"
LINK_CODE_TTL_S = 600
LOC_PREVIEW = "location"


def gchat_audience() -> str:
    """The Chat app's project-number audience -- read fresh from the
    environment on every call, the same per-call `os.environ.get(...)`
    pattern `app/notify/sms.py` uses for Twilio's credentials. Empty in
    dev/test (no real Chat app configured); `verify_chat_bearer_token`
    treats a blank audience as "never verifies" (fail closed)."""
    return os.environ.get("GCHAT_AUDIENCE", "")


def gchat_certs_url() -> str:
    return os.environ.get("GCHAT_CERTS_URL", DEFAULT_CERTS_URL)


def verify_chat_bearer_token(
    token: str,
    *,
    audience: str,
    certs: Mapping[str, str] | None = None,
    certs_url: str | None = None,
) -> dict[str, Any]:
    """Raises `ValueError` on any invalid signature/audience/issuer/expiry
    -- there is no "falsy but not exception" failure mode, so a caller that
    forgets to check a return value still fails safely.

    `certs` (injected -- a plain `{key_id: public_key_or_cert_pem}` map)
    bypasses the network entirely, for test fixtures that sign their own
    JWT with a locally generated key. Left `None` (the real/prod path)
    fetches Google's published certs over HTTPS via
    `google.oauth2.id_token.verify_token` -- the `google-auth` library's
    own standard JWT verification helper."""
    if not audience:
        raise ValueError("no GCHAT_AUDIENCE configured for this deployment")
    # L2: a small clock-skew tolerance -- both verification call sites
    # default to 0, which can produce spurious 401s from sub-second clock
    # drift between Google's signer and this Cloud Run instance's clock.
    if certs is not None:
        claims = google_jwt.decode(
            token, certs=dict(certs), audience=audience, clock_skew_in_seconds=30
        )
    else:
        request = google_requests.Request()
        claims = google_id_token.verify_token(
            token,
            request,
            audience=audience,
            certs_url=certs_url or DEFAULT_CERTS_URL,
            clock_skew_in_seconds=30,
        )
    issuer = claims.get("iss")
    if issuer != CHAT_ISSUER:
        raise ValueError(f"unexpected issuer: {issuer!r}")
    return claims


class GChatConfig(BaseModel):
    model_config = ConfigDict(extra="ignore")

    space: str | None = None


class ChatClient(Protocol):
    """Stubbable stand-in for the real `spaces.messages.create` call."""

    def send_message(self, space: str, text: str) -> str | None: ...


class NullChatClient:
    """Default in dev/test -- never touches the real Chat API (no ADC
    there); matches `webapp.py`'s `NullFCMClient`."""

    def send_message(self, space: str, text: str) -> str | None:
        return None


class GoogleChatClient:
    """Real client: ADC (`google.auth.default()`), no shared secret --
    docs/SERVER_PLAN.md §6.5: "the relay's service account credentials".
    Only ever constructed by a caller that explicitly asks for it
    (`app/backends/registry.py`'s `chat_client` parameter) -- never the
    default, so a dev/test process with no ADC configured never even
    attempts to build one."""

    def __init__(self) -> None:
        from google.auth.transport.requests import AuthorizedSession

        credentials, _ = google_auth.default(
            scopes=["https://www.googleapis.com/auth/chat.bot"]
        )
        self._session = AuthorizedSession(credentials)

    def send_message(self, space: str, text: str) -> str | None:
        resp = self._session.post(
            f"{CHAT_API_BASE_URL}/{space}/messages", json={"text": text}, timeout=5.0
        )
        if 200 <= resp.status_code < 300:
            return resp.json().get("name")
        logger.warning("gchat send rejected (space=%s status=%s)", space, resp.status_code)
        return None


def _render_body(msg: Message) -> str:
    if msg.body:
        return msg.body
    if msg.kind == "loc" and msg.loc is not None:
        return LOC_PREVIEW
    return "(no content)"


class GChatBackend:
    kind = "gchat"
    config_schema = GChatConfig

    def __init__(self, chat_client: ChatClient | None = None) -> None:
        self._chat = chat_client or NullChatClient()

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        space = backend.config.get("space")
        if not space:
            # Not linked yet -- recoverable (a later tick retries once
            # `/link` completes), same shape as pager's missing-deviceId
            # and sms's missing-phone cases.
            return DeliverResult(ok=False, state="queued", error="gchat backend not linked yet")
        text = _render_body(msg)
        try:
            message_name = self._chat.send_message(space, text)
        except Exception as exc:  # noqa: BLE001 -- best-effort delivery, see app/routing.py's docstring
            logger.warning("gchat send failed (space=%s): %r", space, exc)
            return DeliverResult(ok=False, state="queued", error=str(exc))
        messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
        return DeliverResult(ok=True, state="sent", external_id=message_name)

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        code = f"{secrets.randbelow(1_000_000):06d}"
        expires_at = int(time.time()) + LINK_CODE_TTL_S
        backends_store.set_gchat_link_code(code, user.uid, backend.id, expires_at)
        # Also written into this backend's own `config` (readable by its
        # owner today, `firestore.rules`' `users/{uid}/backends/{b}` rule),
        # not just the `gchatLinkCodes` index above -- §6.5 says the web
        # app's `/settings/backends` page *shows the user this code*, and
        # `config` is the one place the page's existing Firestore listener
        # already reads. Note that `web/app/settings/backends/page.tsx`
        # does not render `config.linkCode` for a gchat row yet -- the data
        # is available, displaying it is a small UI follow-up.
        backends_store.update_backend(
            user.uid,
            backend.id,
            config={**backend.config, "linkCode": code, "linkCodeExpiresAt": expires_at},
        )
        return LinkStep(
            instructions=(
                f"Open a DM with the Pager bot in Google Chat and send: /link {code}"
            ),
            expiresInS=LINK_CODE_TTL_S,
        )

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        # See module docstring: real completion happens inside the inbound
        # webhook, which has the (uid, bid) pair straight from
        # `pop_gchat_link_code` and so never calls this. Always False --
        # there is no `POST /api/me/backends/{id}/verify` caller for gchat
        # (the web app's `/settings/backends` page never opens a verify
        # dialog for a gchat row, only sms).
        return False

    def render_state(self, delivery: Delivery) -> str:
        return delivery.state

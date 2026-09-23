"""Real FCM push backend -- docs/SERVER_PLAN.md §7.6, docs/V03_PLAN.md §3a.

`FirebaseFCMClient` is the production implementation of `backends.webapp.
FCMClient`'s `send_data(tokens, data)` seam, using `firebase_admin.
messaging`'s multicast send. Kept out of `webapp.py` itself so that module
(and every existing test that imports it) never has to import
`firebase_admin.messaging` -- that import reaches for real Firebase Admin
SDK state that dev/test has no credentials for, which is exactly why
`PUSH_BACKEND` (`app/config.py`) defaults to `null` and `webapp.py`'s
`NullFCMClient` stays the default there.

Token hygiene: docs/SERVER_PLAN.md §5.7 used to say "removed after 3
consecutive FCM 'unregistered' errors", but nothing ever incremented that
counter (`app/jobs.py`'s module docstring says so explicitly) so the rule
was never implemented. This client instead deletes a token the first time
FCM reports it dead (`UnregisteredError` / `SenderIdMismatchError`) --
three-strike counting buys nothing for a token FCM has already declared
gone. Any other per-token failure (rate limits, transient backend errors,
etc.) is logged at INFO and the token is kept.
"""

from __future__ import annotations

import logging

from firebase_admin import messaging

from app.store import push_tokens as push_tokens_store

logger = logging.getLogger("relay.backends.fcm")

# `send_each_for_multicast` caps at 500 tokens per call (FCM's own limit).
_CHUNK_SIZE = 500

# docs/V03_TASKS.md 3a.1 / docs/V03_PLAN.md §3a: high priority so a phone
# wakes for it, and a multi-hour TTL so a phone that was off still gets the
# latest message once it reconnects.
_WEBPUSH_HEADERS = {"Urgency": "high", "TTL": "14400"}

_DEAD_TOKEN_ERRORS = (messaging.UnregisteredError, messaging.SenderIdMismatchError)


class FirebaseFCMClient:
    """`backends.webapp.FCMClient` implementation backed by real FCM."""

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None:
        for start in range(0, len(tokens), _CHUNK_SIZE):
            self._send_chunk(tokens[start : start + _CHUNK_SIZE], data)

    def _send_chunk(self, tokens: list[str], data: dict[str, str]) -> None:
        message = messaging.MulticastMessage(
            tokens=tokens,
            data=data,
            webpush=messaging.WebpushConfig(headers=dict(_WEBPUSH_HEADERS)),
        )
        response = messaging.send_each_for_multicast(message)
        for token, result in zip(tokens, response.responses, strict=True):
            if result.success:
                continue
            exc = result.exception
            if isinstance(exc, _DEAD_TOKEN_ERRORS):
                push_tokens_store.remove_token_anywhere(token)
            else:
                # Log a prefix only: a push token is a capability to notify
                # that browser, so keep the full value out of the logs.
                logger.info("FCM send failed for token %s…: %s", token[:12], exc)

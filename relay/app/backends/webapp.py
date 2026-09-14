"""`webapp` backend -- docs/SERVER_PLAN.md §6.3.

`deliver()` is nearly free: the message document `routing.py` already wrote
*is* the delivery (the browser's Firestore listener sees it immediately),
so all this does is mark the delivery `sent` and best-effort push an FCM
data message to the recipient's registered tokens. FCM itself is injected
as a small `FCMClient` protocol so tests (and dev, which has no real FCM
credentials) can substitute a no-op/fake rather than this module reaching
for real `firebase_admin.messaging` (docs/SERVER_PLAN.md §5.9: "FCM sends
go through a stub `messaging` client").

Delivery goes `read` separately, via `POST /api/conversations/{alias}/
messages/{id}/read` (`app/routers/conversations.py`), when the browser
reports the thread viewed -- not from this module.
"""

from __future__ import annotations

import logging
from typing import Protocol

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.store import messages as messages_store
from app.store import push_tokens as push_tokens_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.webapp")

PREVIEW_MAX_CHARS = 120


class WebappConfig(BaseModel):
    """Every user gets an implicit `webapp` backend at creation
    (`app/store/users.py`'s `create_user`); there is nothing for a user to
    configure."""

    model_config = ConfigDict(extra="ignore")


class FCMClient(Protocol):
    """Stubbable stand-in for `firebase_admin.messaging`."""

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None: ...


class NullFCMClient:
    """Default FCM client: does nothing. Never touches real Firebase Cloud
    Messaging -- no credentials for it are configured in dev/test."""

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None:
        return None


class WebappBackend:
    kind = "webapp"
    config_schema = WebappConfig

    def __init__(self, fcm_client: FCMClient | None = None) -> None:
        self._fcm = fcm_client or NullFCMClient()

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
        try:
            tokens = push_tokens_store.list_tokens(msg.recipientUid)
            if tokens:
                self._fcm.send_data(
                    tokens,
                    {
                        "convKey": msg.convKey,
                        "id": msg.id,
                        "senderUid": msg.senderUid,
                        "body": (msg.body or "")[:PREVIEW_MAX_CHARS],
                    },
                )
        except Exception:
            # FCM is best-effort: the message is already visible to the
            # browser's Firestore listener regardless of whether the push
            # notification itself succeeds, so a push failure must not fail
            # the delivery.
            logger.exception("FCM push failed for message %s", msg.id)
        return DeliverResult(ok=True, state="sent")

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return True

    def render_state(self, delivery: Delivery) -> str:
        return delivery.state

"""`kind` -> `Backend` implementation -- docs/SERVER_PLAN.md §6.1: "A new
backend is one module + one line in `registry.py`"."""

from __future__ import annotations

from app.backends.base import Backend
from app.backends.gchat import ChatClient, GChatBackend
from app.backends.pager import PagerBackend
from app.backends.sms_twilio import SmsTwilioBackend
from app.backends.webapp import FCMClient, WebappBackend
from app.broker import BrokerClient


def build_registry(
    broker: BrokerClient,
    *,
    fcm_client: FCMClient | None = None,
    chat_client: ChatClient | None = None,
) -> dict[str, Backend]:
    return {
        "pager": PagerBackend(broker),
        "webapp": WebappBackend(fcm_client),
        # Phase 7: real Twilio adapter (docs/SERVER_PLAN.md §6.4) --
        # replaces the Phase 5 stub (app/backends/sms_stub.py, deleted).
        "sms": SmsTwilioBackend(),
        # Phase 7: real Google Chat adapter (docs/SERVER_PLAN.md §6.5).
        # `chat_client` stays `None` (-> `NullChatClient`) in dev/test,
        # same "inject the real client explicitly" pattern `fcm_client`
        # already established.
        "gchat": GChatBackend(chat_client),
    }

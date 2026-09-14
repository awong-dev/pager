"""`kind` -> `Backend` implementation -- docs/SERVER_PLAN.md §6.1: "A new
backend is one module + one line in `registry.py`"."""

from __future__ import annotations

from app.backends.base import Backend
from app.backends.pager import PagerBackend
from app.backends.sms_stub import SmsStubBackend
from app.backends.webapp import FCMClient, WebappBackend
from app.broker import BrokerClient


def build_registry(broker: BrokerClient, *, fcm_client: FCMClient | None = None) -> dict[str, Backend]:
    return {
        "pager": PagerBackend(broker),
        "webapp": WebappBackend(fcm_client),
        # Phase 5 stub -- see app/backends/sms_stub.py's module docstring
        # (TODO(orchestrator): replace with the real Twilio adapter in
        # Phase 7).
        "sms": SmsStubBackend(),
    }

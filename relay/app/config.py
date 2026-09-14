"""Runtime configuration loaded strictly from environment variables.

See relay/.env.example for the documented set of variables. Nothing here has
a real secret default -- RELAY_TOKEN and WEBHOOK_KEY default to "" so a
misconfigured deployment fails closed (every request gets 401) rather than
open.
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Settings:
    relay_token: str
    # docs/SERVER_PLAN.md §2 decision 1 / §4.8: the relay never holds an MQTT
    # connection. Device traffic arrives via POST /webhooks/mqtt (the
    # broker's rule engine); the relay publishes /down via the broker's REST
    # publish API at `broker_api_url` (e.g. EMQX's `/api/v5`), authenticated
    # with `broker_api_key`/`broker_api_secret` (HTTP basic auth).
    broker_api_url: str
    broker_api_key: str | None
    broker_api_secret: str | None
    # Shared secret the broker's rule-engine HTTP action sends back on every
    # webhook call (`X-Relay-Webhook-Key`), so nobody else can post "device
    # traffic" (§5.3).
    webhook_key: str
    db_path: str

    @classmethod
    def from_env(cls) -> Settings:
        return cls(
            relay_token=os.environ.get("RELAY_TOKEN", ""),
            broker_api_url=os.environ.get(
                "BROKER_API_URL", "http://localhost:18083/api/v5"
            ),
            broker_api_key=os.environ.get("BROKER_API_KEY") or None,
            broker_api_secret=os.environ.get("BROKER_API_SECRET") or None,
            webhook_key=os.environ.get("WEBHOOK_KEY", ""),
            db_path=os.environ.get("RELAY_DB_PATH", "relay.db"),
        )

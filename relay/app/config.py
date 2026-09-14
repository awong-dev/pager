"""Runtime configuration loaded strictly from environment variables.

See relay/.env.example for the documented set of variables. Nothing here has
a real secret default -- WEBHOOK_KEY defaults to "" so a misconfigured
deployment fails closed (every request gets 401) rather than open.
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Settings:
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
    # docs/SERVER_PLAN.md §5.1/§5.3: gates `POST /api/dev/token`, the
    # DEV_MODE-only custom-token mint used by the test client
    # (tools/pager_client.py) to sign in without a real email/phone flow.
    # Never true in a real deployment.
    dev_mode: bool
    # google-cloud-firestore / firebase_admin.auth both read
    # FIRESTORE_EMULATOR_HOST / FIREBASE_AUTH_EMULATOR_HOST directly and
    # firebase_admin reads GOOGLE_CLOUD_PROJECT itself (app/db/firestore.py)
    # -- surfaced here too only so `/healthz` and logs can report what the
    # process thinks it's pointed at.
    google_cloud_project: str | None
    firestore_emulator_host: str | None
    firebase_auth_emulator_host: str | None

    @classmethod
    def from_env(cls) -> Settings:
        return cls(
            broker_api_url=os.environ.get(
                "BROKER_API_URL", "http://localhost:18083/api/v5"
            ),
            broker_api_key=os.environ.get("BROKER_API_KEY") or None,
            broker_api_secret=os.environ.get("BROKER_API_SECRET") or None,
            webhook_key=os.environ.get("WEBHOOK_KEY", ""),
            dev_mode=os.environ.get("DEV_MODE", "") == "1",
            google_cloud_project=os.environ.get("GOOGLE_CLOUD_PROJECT") or None,
            firestore_emulator_host=os.environ.get("FIRESTORE_EMULATOR_HOST") or None,
            firebase_auth_emulator_host=os.environ.get("FIREBASE_AUTH_EMULATOR_HOST") or None,
        )

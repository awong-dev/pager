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
    # docs/DEVICE_PLAN.md §3.3 (docs/DEVICE_TASKS.md S2b.2): the production
    # MQTT/TLS host bootstrap bundles point the device at, and the host
    # `app/ca_resolve.py` connects to on port 8883 to auto-resolve the
    # broker's CA when BROKER_CA_PEM (below) is unset. Also read directly
    # from the environment today by `app/routers/admin.py`'s
    # `_bootstrap_host_and_ca` (that module's own comment names this task as
    # the one that would give it a proper home; admin.py is mid-edit by a
    # concurrent task as of this change, so it still reads `os.environ`
    # itself instead of `Settings.broker_host`).
    # Defaulted (unlike every field above) so the many `Settings(**defaults)`
    # test fixtures across `relay/tests/` -- none of which are in this
    # task's `Files` list, so they cannot be updated to pass these two
    # explicitly -- keep constructing without them.
    broker_host: str = "localhost"
    # A deployment's Terraform (`infra/`) knows which broker it stood up and
    # sets this to that broker's CA PEM directly -- wins over auto-resolve
    # unconditionally (§3.3: "otherwise resolved once at startup"). Empty
    # means `app/ca_resolve.py` falls back to the TLS-handshake-plus-certifi
    # lookup; if that also fails, bundles carry no `ca` (the device reports
    # "broker certificate not trusted").
    broker_ca_pem: str | None = None
    # docs/V02_DESIGN.md §4.4: the relay's own public base URL, used to build
    # the content-addressed `GET /ca/{sha256hex}.pem` pointer a bootstrap
    # bundle or a `cfg.ca` push hands a device (`https://<this>/ca/<sha>.pem`).
    # Cloud Run v2 cannot reference a service's own URL from inside its own
    # `terraform apply` (a genuine cyclic reference), so this is a plain env
    # var/Terraform variable (`infra/modules/relay-service`), not something
    # derived at runtime. Empty means "no public URL configured" -- when a CA
    # *is* configured, `app/devsetup.py`/the CA-push admin route refuse to
    # issue a setup code/push rather than silently sending an unpinned
    # bundle (docs/V02_DESIGN.md §4.4's "refuse ... with a clear error").
    public_base_url: str | None = None

    @classmethod
    def from_env(cls) -> Settings:
        return cls(
            broker_api_url=os.environ.get(
                "BROKER_API_URL", "http://localhost:18083/api/v5"
            ),
            # .strip(): Secret Manager hands back exactly the bytes that were
            # stored, and `echo value | gcloud secrets versions add` stores a
            # trailing newline. An HTTP header can never carry one, so an
            # unstripped WEBHOOK_KEY could never match and every broker
            # webhook got 401 (found live, 2026-09-20).
            broker_api_key=(os.environ.get("BROKER_API_KEY") or "").strip() or None,
            broker_api_secret=(os.environ.get("BROKER_API_SECRET") or "").strip() or None,
            webhook_key=os.environ.get("WEBHOOK_KEY", "").strip(),
            dev_mode=os.environ.get("DEV_MODE", "") == "1",
            google_cloud_project=os.environ.get("GOOGLE_CLOUD_PROJECT") or None,
            firestore_emulator_host=os.environ.get("FIRESTORE_EMULATOR_HOST") or None,
            firebase_auth_emulator_host=os.environ.get("FIREBASE_AUTH_EMULATOR_HOST") or None,
            broker_host=os.environ.get("BROKER_HOST", "localhost"),
            broker_ca_pem=os.environ.get("BROKER_CA_PEM") or None,
            public_base_url=(os.environ.get("PUBLIC_BASE_URL") or "").rstrip("/") or None,
        )

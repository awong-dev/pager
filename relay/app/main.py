"""FastAPI app: the relay's HTTP surface. Request-driven end to end (no
background thread, no persistent MQTT session, docs/SERVER_PLAN.md §2
decision 1) -- inbound device traffic arrives on `POST /webhooks/mqtt`
(app/routers/webhooks.py) and outbound `/down` publishes go through
`app.broker.BrokerClient`'s REST call. Firestore is the only store
(app/db/firestore.py, app/store/*) -- the relay is its only writer
(docs/SERVER_PLAN.md §2 decision 3).
"""

from __future__ import annotations

import logging
import os
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from app.broker import BrokerClient
from app.config import Settings
from app.db.firestore import get_db
from app.ingest import Ingest
from app.location import Location
from app.routers import admin, conversations, dev, internal, legacy, me, webhooks
from app.routing import Routing

logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"))
logger = logging.getLogger("relay.api")

# relay/static/index.html — the single-file parent MVP page (HANDOFF.md §2,
# §3 repo layout). Resolved relative to this file so it works both from a
# checkout (`relay/app/main.py` -> `relay/static`) and from the Docker image
# (see Dockerfile, which copies `static/` alongside `app/`).
STATIC_DIR = Path(__file__).resolve().parent.parent / "static"


def create_app(
    settings: Settings | None = None,
    broker_client: BrokerClient | None = None,
) -> FastAPI:
    """Factory so tests can inject a fake `BrokerClient` instead of a real
    broker connection. Firestore itself is not injected -- every `app/store/
    *` module talks to the one process-wide client from
    `app.db.firestore.get_db()`, which is emulator-aware via
    `FIRESTORE_EMULATOR_HOST`/`FIREBASE_AUTH_EMULATOR_HOST` (tests point
    those at the Docker-composed emulators, see relay/tests/conftest.py)."""

    settings = settings or Settings.from_env()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        app.state.settings = settings
        app.state.broker = broker_client or BrokerClient(settings)
        # One Routing instance (and, inside it, one backend registry) per
        # app -- shared by the webhook path (via Ingest) and every API
        # router that sends a message, so `app/backends/pager.py`'s
        # `BrokerClient` and `app/backends/webapp.py`'s FCM client are each
        # constructed exactly once.
        app.state.routing = Routing(app.state.broker)
        app.state.ingest = Ingest(app.state.broker, app.state.routing)
        # docs/PROTOCOL.md §13 / docs/SERVER_PLAN.md §5.6: shares the same
        # `Routing` instance so a freshly-claimed `loc_req`'s inline
        # delivery (`Routing.redeliver_pager`) goes through the one backend
        # registry the app constructed.
        app.state.location = Location(app.state.routing)
        get_db()  # fail fast at startup if Firestore/Auth are misconfigured
        yield

    app = FastAPI(title="School Pager Relay", lifespan=lifespan)
    app.include_router(webhooks.router)
    app.include_router(legacy.router)
    app.include_router(admin.router)
    app.include_router(dev.router)
    app.include_router(conversations.router)
    app.include_router(me.router)
    app.include_router(internal.router)

    @app.get("/healthz")
    def healthz() -> dict[str, bool]:
        # docs/SERVER_PLAN.md §5.1: "200 + firestore reachable + broker API
        # reachable". Broker reachability is not checked here (a broker
        # blip must not flip Cloud Run's health probe and cause a
        # restart-loop of a stateless service that holds no connection to
        # lose) -- a Firestore round trip is enough to prove the process can
        # do its job.
        get_db().collection("settings").document("meta").get()
        return {"ok": True}

    # Mounted last so it never shadows the /api/* or /webhooks/* routes
    # above: Starlette matches routes in registration order, and a Mount("/")
    # only catches what no earlier explicit route claimed. Serves
    # relay/static/index.html (the parent MVP page, HANDOFF.md §2) at "/"
    # and its own path.
    if STATIC_DIR.is_dir():
        app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="static")

    return app


app = create_app()

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
import uuid
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from app.backends.registry import build_registry
from app.broker import BrokerClient
from app.config import Settings
from app.db.firestore import get_db
from app.ingest import Ingest
from app.location import Location
from app.logging_config import configure_logging, request_id_var
from app.routers import admin, conversations, dev, internal, me, webhooks
from app.routing import Routing

# JSON logs with a `severity` field, shaped for Cloud Logging's
# structured-log ingestion -- `app/logging_config.py`'s module docstring.
configure_logging(os.environ.get("LOG_LEVEL", "INFO"))
logger = logging.getLogger("relay.api")

REQUEST_ID_HEADER = "X-Request-Id"


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
        # One backend registry per app, built once here (not inside
        # `Routing.__init__`'s own default) so `app/routers/me.py` can also
        # reach it -- `POST /api/me/backends` needs to call a fresh
        # backend's `start_link()` (docs/SERVER_PLAN.md §6.1: "e.g. send a
        # code") right after creating it, which is not something
        # `Routing`'s `send()`-only surface exposes.
        app.state.backend_registry = build_registry(app.state.broker)
        # One Routing instance per app -- shared by the webhook path (via
        # Ingest) and every API router that sends a message, so
        # `app/backends/pager.py`'s `BrokerClient` and
        # `app/backends/webapp.py`'s FCM client are each constructed
        # exactly once.
        app.state.routing = Routing(app.state.broker, registry=app.state.backend_registry)
        app.state.ingest = Ingest(app.state.broker, app.state.routing)
        # docs/PROTOCOL.md §13 / docs/SERVER_PLAN.md §5.6: shares the same
        # `Routing` instance so a freshly-claimed `loc_req`'s inline
        # delivery (`Routing.redeliver_pager`) goes through the one backend
        # registry the app constructed.
        app.state.location = Location(app.state.routing)
        get_db()  # fail fast at startup if Firestore/Auth are misconfigured
        yield

    app = FastAPI(title="School Pager Relay", lifespan=lifespan)

    # A per-request correlation id, threaded through every log line
    # emitted while handling this
    # request (`app/logging_config.py`'s `request_id_var`) -- the caller's
    # own `X-Request-Id`, if it sent one (Cloud Run/Hosting/a load balancer
    # commonly does), else a freshly generated one, echoed back on the
    # response so a client can correlate its own logs against the relay's.
    @app.middleware("http")
    async def _request_id_middleware(request: Request, call_next):
        request_id = request.headers.get(REQUEST_ID_HEADER) or uuid.uuid4().hex
        token = request_id_var.set(request_id)
        try:
            response = await call_next(request)
        finally:
            request_id_var.reset(token)
        response.headers[REQUEST_ID_HEADER] = request_id
        return response

    app.include_router(webhooks.router)
    app.include_router(admin.router)
    app.include_router(dev.router)
    app.include_router(conversations.router)
    app.include_router(me.router)
    app.include_router(internal.router)

    @app.get("/healthz")
    def healthz() -> JSONResponse:
        # docs/SERVER_PLAN.md §5.1: "200 + firestore reachable + broker API
        # reachable" -- both checks, and a non-200 when either is down, with
        # a body naming which one failed. Checking the broker here is safe
        # because nothing in `infra/modules/relay-service` wires this route
        # as a Cloud Run liveness/startup probe (no `liveness_probe` block
        # exists there), so a broker blip cannot restart-loop the service;
        # this endpoint is an external uptime check / manual curl.
        firestore_ok = True
        try:
            get_db().collection("settings").document("meta").get()
        except Exception:
            logger.exception("healthz: firestore check failed")
            firestore_ok = False

        broker_ok = True
        try:
            broker_ok = app.state.broker.healthcheck()
        except Exception:
            logger.exception("healthz: broker check failed")
            broker_ok = False

        ok = firestore_ok and broker_ok
        body = {"ok": ok, "firestore": firestore_ok, "broker": broker_ok}
        return JSONResponse(status_code=200 if ok else 503, content=body)

    return app


app = create_app()

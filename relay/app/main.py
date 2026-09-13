"""FastAPI app: the relay's HTTP surface. Owns nothing about MQTT wire
details directly -- it validates/generates ids per PROTOCOL.md §1/§3.1,
stores via Store, and publishes via MqttGateway.
"""

from __future__ import annotations

import logging
import os
import secrets
import time
from collections.abc import Callable
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated

from fastapi import Depends, FastAPI, HTTPException, Query, Request
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ConfigDict, Field

from app.config import Settings
from app.ids import new_message_id
from app.mqtt_gateway import MqttGateway
from app.mqtt_transport import PahoTransport
from app.store import MessageRow, Store
from app.wire import (
    BODY_MAX_CODEPOINTS,
    BODY_MAX_UTF8_BYTES,
    DEVICE_ID_RE,
    strip_control_chars,
)

logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"))
logger = logging.getLogger("relay.api")

bearer_scheme = HTTPBearer(auto_error=False)

MAX_ID_GENERATION_ATTEMPTS = 10

# relay/static/index.html — the single-file parent MVP page (HANDOFF.md §2,
# §3 repo layout). Resolved relative to this file so it works both from a
# checkout (`relay/app/main.py` -> `relay/static`) and from the Docker image
# (see Dockerfile, which copies `static/` alongside `app/`).
STATIC_DIR = Path(__file__).resolve().parent.parent / "static"


class SendMessageRequest(BaseModel):
    body: str


class MessageOut(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    id: str
    device_id: str
    direction: str
    ts: int
    from_: str | None = Field(default=None, alias="from")
    body: str | None = None
    state: str
    shown_ts: int | None = None
    read_ts: int | None = None


class StatusOut(BaseModel):
    state: str
    mode: str | None = None
    batt_mv: int | None = None
    rssi: int | None = None
    session: str | None = None
    ts: int | None = None
    fw: str | None = None


def _row_to_message_out(row: MessageRow) -> MessageOut:
    return MessageOut.model_validate(
        {
            "id": row.id,
            "device_id": row.device_id,
            "direction": row.direction,
            "ts": row.ts,
            "from": row.sender,
            "body": row.body,
            "state": row.effective_state(),
            "shown_ts": row.shown_ts,
            "read_ts": row.read_ts,
        }
    )


GatewayFactory = Callable[[Store], MqttGateway]


def get_store(request: Request) -> Store:
    return request.app.state.store


def get_gateway(request: Request) -> MqttGateway:
    return request.app.state.gateway


def require_auth(
    request: Request,
    creds: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer_scheme)] = None,
) -> None:
    settings: Settings = request.app.state.settings
    if creds is None or not secrets.compare_digest(creds.credentials, settings.relay_token):
        raise HTTPException(status_code=401, detail="invalid or missing bearer token")


def create_app(
    settings: Settings | None = None,
    store: Store | None = None,
    gateway_factory: GatewayFactory | None = None,
) -> FastAPI:
    """Factory so tests can inject an in-memory Store and a fake-transport
    MqttGateway instead of a real broker connection."""

    settings = settings or Settings.from_env()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        app.state.settings = settings
        app.state.store = store or Store(settings.db_path)
        if gateway_factory is not None:
            app.state.gateway = gateway_factory(app.state.store)
        else:
            transport = PahoTransport(
                client_id="relay-1",
                host=settings.mqtt_host,
                port=settings.mqtt_port,
                username=settings.mqtt_username,
                password=settings.mqtt_password,
            )
            app.state.gateway = MqttGateway(app.state.store, transport)
        app.state.gateway.start()
        try:
            yield
        finally:
            app.state.gateway.stop()
            app.state.store.close()

    app = FastAPI(title="School Pager Relay", lifespan=lifespan)

    @app.post("/api/devices/{device_id}/messages", dependencies=[Depends(require_auth)])
    def send_message(
        device_id: str,
        req: SendMessageRequest,
        store: Annotated[Store, Depends(get_store)],
        gateway: Annotated[MqttGateway, Depends(get_gateway)],
    ) -> MessageOut:
        if not DEVICE_ID_RE.match(device_id):
            raise HTTPException(status_code=400, detail="invalid device_id")

        # §3.1: strip control chars on ingest, reject if empty or oversize
        # after stripping.
        body = strip_control_chars(req.body)
        if not body:
            raise HTTPException(
                status_code=400, detail="body is empty after stripping control characters"
            )
        if len(body) > BODY_MAX_CODEPOINTS:
            raise HTTPException(status_code=400, detail="body exceeds 160 Unicode code points")
        if len(body.encode("utf-8")) > BODY_MAX_UTF8_BYTES:
            raise HTTPException(status_code=400, detail="body exceeds 320 UTF-8 bytes")

        ts = int(time.time())
        msg_id = None
        for _ in range(MAX_ID_GENERATION_ATTEMPTS):
            candidate = new_message_id()
            if not store.id_exists(candidate):
                msg_id = candidate
                break
        if msg_id is None:
            raise HTTPException(status_code=500, detail="failed to generate a unique message id")

        row = store.create_down_message(msg_id=msg_id, device_id=device_id, ts=ts, body=body)
        gateway.publish_down(row)
        return _row_to_message_out(row)

    @app.get("/api/devices/{device_id}/messages", dependencies=[Depends(require_auth)])
    def get_messages(
        device_id: str,
        store: Annotated[Store, Depends(get_store)],
        since: int | None = Query(default=None),
    ) -> list[MessageOut]:
        rows = store.get_thread(device_id, since=since)
        return [_row_to_message_out(r) for r in rows]

    @app.get("/api/devices/{device_id}/status", dependencies=[Depends(require_auth)])
    def get_status(
        device_id: str,
        store: Annotated[Store, Depends(get_store)],
    ) -> StatusOut:
        status = store.get_status(device_id)
        if status is None:
            raise HTTPException(status_code=404, detail="no status seen for this device")
        return StatusOut(
            state=status.state,
            mode=status.mode,
            batt_mv=status.batt_mv,
            rssi=status.rssi,
            session=status.session,
            ts=status.ts,
            fw=status.fw,
        )

    # Mounted last so it never shadows the /api/* routes above: Starlette
    # matches routes in registration order, and a Mount("/") only catches
    # what no earlier explicit route claimed. Serves relay/static/index.html
    # (the parent MVP page, HANDOFF.md §2) at "/" and its own path.
    if STATIC_DIR.is_dir():
        app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="static")

    return app


app = create_app()

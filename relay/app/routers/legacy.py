"""Legacy `RELAY_TOKEN`-bearer endpoints from the MVP
(`POST/GET /api/devices/{device_id}/messages`, `GET /api/devices/{device_id}/status`),
docs/SERVER_PLAN.md §5.1: "Legacy (bearer RELAY_TOKEN, deleted in Phase 6)".
Repointed at `app/store/legacy.py`'s Firestore-backed device-scoped thread
(see that module's docstring for why it is not the uid-addressed
`messages/{id}` model) -- same external behavior as the MVP, new storage
engine underneath.
"""

from __future__ import annotations

import secrets
import time
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pydantic import BaseModel, ConfigDict, Field

from app.config import Settings
from app.ingest import Ingest
from app.store import legacy as legacy_store
from app.store.legacy import LegacyMessage
from app.wire import (
    BODY_MAX_CODEPOINTS,
    BODY_MAX_UTF8_BYTES,
    DEVICE_ID_RE,
    strip_control_chars,
)

router = APIRouter()
bearer_scheme = HTTPBearer(auto_error=False)

MAX_ID_GENERATION_ATTEMPTS = 10


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


def _row_to_message_out(row: LegacyMessage) -> MessageOut:
    return MessageOut.model_validate(
        {
            "id": row.id,
            "device_id": row.deviceId,
            "direction": row.direction,
            "ts": row.ts,
            "from": row.sender,
            "body": row.body,
            "state": row.effective_state(),
            "shown_ts": row.shownTs,
            "read_ts": row.readTs,
        }
    )


def get_ingest(request: Request) -> Ingest:
    return request.app.state.ingest


def require_auth(
    request: Request,
    creds: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer_scheme)] = None,
) -> None:
    settings: Settings = request.app.state.settings
    if creds is None or not secrets.compare_digest(creds.credentials, settings.relay_token):
        raise HTTPException(status_code=401, detail="invalid or missing bearer token")


@router.post("/api/devices/{device_id}/messages", dependencies=[Depends(require_auth)])
def send_message(
    device_id: str,
    req: SendMessageRequest,
    ingest: Annotated[Ingest, Depends(get_ingest)],
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
        candidate = legacy_store.new_legacy_message_id()
        if not legacy_store.id_exists(candidate):
            msg_id = candidate
            break
    if msg_id is None:
        raise HTTPException(status_code=500, detail="failed to generate a unique message id")

    row = legacy_store.create_down_message(msg_id=msg_id, device_id=device_id, ts=ts, body=body)
    ingest.publish_down(row)
    return _row_to_message_out(legacy_store.get_message(msg_id) or row)


@router.get("/api/devices/{device_id}/messages", dependencies=[Depends(require_auth)])
def get_messages(
    device_id: str,
    ingest: Annotated[Ingest, Depends(get_ingest)],
    since: int | None = Query(default=None),
) -> list[MessageOut]:
    # Lazy retry of anything still 'queued' -- see Ingest.retry_queued's
    # docstring for why this lives here rather than a background timer.
    ingest.retry_queued(device_id)
    rows = legacy_store.get_thread(device_id, since=since)
    return [_row_to_message_out(r) for r in rows]


@router.get("/api/devices/{device_id}/status", dependencies=[Depends(require_auth)])
def get_status(device_id: str) -> StatusOut:
    status = legacy_store.get_status(device_id)
    if status is None:
        raise HTTPException(status_code=404, detail="no status seen for this device")
    return StatusOut(
        state=status.state,
        mode=status.mode,
        batt_mv=status.battMv,
        rssi=status.rssi,
        session=status.session,
        ts=status.ts,
        fw=status.fw,
    )

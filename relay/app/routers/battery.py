"""Battery time series + model store -- docs/BATTERY_STATS_DESIGN.md B6."""

from __future__ import annotations

import re
import time
from typing import Annotated, Any

from fastapi import APIRouter, Depends, HTTPException, Query

from app import battmodel
from app.auth import AuthedUser, require_super, require_user
from app.db.firestore import get_db
from app.routers.devices import _require_owner_or_admin
from app.store import battery as battery_store

router = APIRouter(prefix="/api")

_MODEL_ID_RE = re.compile(r"^[a-z0-9-]{1,40}$")
MAX_SPAN_S = 90 * 86400


@router.get("/devices/{device_id}/battery")
def get_battery(
    device_id: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
    since: Annotated[int | None, Query()] = None,
    until: Annotated[int | None, Query()] = None,
) -> dict[str, Any]:
    _require_owner_or_admin(device_id, authed)
    until_v = until if until is not None else int(time.time())
    since_v = since if since is not None else until_v - 7 * 86400
    if since_v > until_v:
        raise HTTPException(status_code=422, detail="since must not exceed until")
    if until_v - since_v > MAX_SPAN_S:
        raise HTTPException(status_code=422, detail="span must be at most 90 days")
    samples, truncated = battery_store.list_samples(device_id, since_v, until_v)
    raw = get_db().collection("devices").document(device_id).get().to_dict() or {}
    model = battmodel.get_model(raw.get("batteryModel") or battmodel.DEFAULT_MODEL_ID)
    return {
        "deviceId": device_id,
        "since": since_v,
        "until": until_v,
        "samples": samples,
        "truncated": truncated,
        "model": model,
    }


@router.put("/admin/battery-models/{model_id}")
def put_battery_model(
    model_id: str,
    body: battmodel.BatteryModel,
    authed: Annotated[AuthedUser, Depends(require_super)],
) -> dict[str, Any]:
    if not _MODEL_ID_RE.fullmatch(model_id):
        raise HTTPException(status_code=422, detail="bad model id")
    return battmodel.put_model(model_id, body, authed.uid)

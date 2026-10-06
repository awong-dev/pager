"""`devices/{deviceId}/battery/{session}-{sq}` -- docs/BATTERY_STATS_DESIGN.md B5/B6.

One sample per online `/status`. A `/status` with a valid `bs` sub-map is
keyed `{session}-{sq}` (a `set`, so a QoS 1 redelivery or a re-sent window
overwrites); one without gets an auto id, so older firmware still produces a
voltage series.
"""

from __future__ import annotations

import logging
import time
from datetime import UTC, datetime
from typing import Annotated, Any

from google.cloud.firestore_v1.base_query import FieldFilter
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from app.db.firestore import get_db
from app.wire import StatusEnvelope

logger = logging.getLogger(__name__)

U32 = Annotated[int, Field(ge=0, le=2**32 - 1, strict=True)]


class BattStats(BaseModel):
    model_config = ConfigDict(extra="ignore")

    sq: U32
    dt: U32
    sl: U32
    aw: list[U32] = Field(min_length=6)
    ns: U32
    x1: U32
    rl: U32
    rf: list[U32] = Field(min_length=3)
    mvn: int | None = Field(None, ge=2000, le=4500, strict=True)
    cn: U32
    md: list[U32] = Field(min_length=3)
    re: U32


def parse_bs(raw: Any, device_id: str = "?") -> BattStats | None:
    if raw is None:
        return None
    try:
        return BattStats.model_validate(raw)
    except ValidationError as exc:
        logger.warning("battery bs malformed device=%s: %s", device_id, exc)
        return None


def _battery(device_id: str):
    return get_db().collection("devices").document(device_id).collection("battery")


def add_sample(
    device_id: str, env: StatusEnvelope, resolved_ts: int | None, bs: BattStats | None
) -> str:
    now = int(time.time())
    doc: dict[str, Any] = {
        "ts": resolved_ts if resolved_ts else now,
        "createdAt": datetime.now(UTC),
        "session": env.session,
        "battMv": env.batt_mv,
        "rssi": env.rssi,
        "mode": env.mode,
        "xport": env.xport,
        "fw": env.fw,
        "img": env.img,
        "rst": env.rst,
        "link": env.link,
        "hasBs": bs is not None,
    }
    if bs is not None:
        aw = bs.aw[:6]
        rf = bs.rf[:3]
        md = bs.md[:3]
        doc.update(
            sq=bs.sq,
            minMv=bs.mvn,
            dtS=bs.dt,
            sleepS=bs.sl,
            awakeS=dict(zip(("timer", "attn", "hot", "ui", "modem", "fetch"), aw, strict=True)),
            sleeps=bs.ns,
            ext1=bs.x1,
            railS=bs.rl,
            refresh=dict(zip(("full", "partial", "upgraded"), rf, strict=True)),
            connects=bs.cn,
            modemS=dict(zip(("off", "search", "gnss"), md, strict=True)),
            radioEvents=bs.re,
        )
    doc = {k: v for k, v in doc.items() if v is not None}
    col = _battery(device_id)
    if bs is not None:
        doc_id = f"{env.session}-{bs.sq}".replace("/", "_")
        col.document(doc_id).set(doc)
        return doc_id
    _, ref = col.add(doc)
    return ref.id


def list_samples(
    device_id: str, since: int, until: int, limit: int = 5000
) -> tuple[list[dict[str, Any]], bool]:
    query = (
        _battery(device_id)
        .where(filter=FieldFilter("ts", ">=", since))
        .where(filter=FieldFilter("ts", "<=", until))
        .order_by("ts")
        .limit(limit + 1)
    )
    out: list[dict[str, Any]] = []
    for snap in query.stream():
        d = snap.to_dict() or {}
        d.pop("createdAt", None)
        out.append({"id": snap.id, **d})
    truncated = len(out) > limit
    return out[:limit], truncated

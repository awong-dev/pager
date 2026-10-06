"""Battery current model -- docs/BATTERY_STATS_DESIGN.md §3, B6.

`batteryModels/{modelId}` holds the per-hardware currents. A missing doc
returns the priors (`source: "prior"`). `model_mah` evaluates the §3 formula
over one stored `devices/{id}/battery` sample.
"""

from __future__ import annotations

import time
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field

DEFAULT_MODEL_ID = "walter-6092-lipo2500"

CAUSES = ("timer", "attn", "hot", "ui", "modem", "fetch")

PRIORS: dict[str, Any] = {
    "sleep_mA": 1.0,
    "awake_mA": {"timer": 40, "attn": 40, "hot": 40, "ui": 40, "modem": 40, "fetch": 120},
    "rail_mA": 5.0,
    "modemIdle_mA": 0.35,
    "modemOff_mA": 0.03,
    "modemSearch_mA": 20.0,
    "gnss_mA": 30.0,
    "radioEvent_mAh": 0.10,
    "connect_mAh": 0.20,
    "fullRefresh_mAh": 0.010,
    "partialRefresh_mAh": 0.001,
    "capacityMah": 2500,
    "usableFrac": 0.95,
}

_MA = Field(ge=0, le=500)
_MAH = Field(ge=0, le=5)


class AwakeCurrents(BaseModel):
    model_config = ConfigDict(extra="forbid")

    timer: float = _MA
    attn: float = _MA
    hot: float = _MA
    ui: float = _MA
    modem: float = _MA
    fetch: float = _MA


class BatteryModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    sleep_mA: float = Field(PRIORS["sleep_mA"], ge=0, le=500)
    awake_mA: AwakeCurrents = AwakeCurrents.model_validate(PRIORS["awake_mA"])
    rail_mA: float = Field(PRIORS["rail_mA"], ge=0, le=500)
    modemIdle_mA: float = Field(PRIORS["modemIdle_mA"], ge=0, le=500)
    modemOff_mA: float = Field(PRIORS["modemOff_mA"], ge=0, le=500)
    modemSearch_mA: float = Field(PRIORS["modemSearch_mA"], ge=0, le=500)
    gnss_mA: float = Field(PRIORS["gnss_mA"], ge=0, le=500)
    radioEvent_mAh: float = Field(PRIORS["radioEvent_mAh"], ge=0, le=5)
    connect_mAh: float = Field(PRIORS["connect_mAh"], ge=0, le=5)
    fullRefresh_mAh: float = Field(PRIORS["fullRefresh_mAh"], ge=0, le=5)
    partialRefresh_mAh: float = Field(PRIORS["partialRefresh_mAh"], ge=0, le=5)
    capacityMah: int = Field(PRIORS["capacityMah"], ge=100, le=10000)
    usableFrac: float = Field(PRIORS["usableFrac"], ge=0.5, le=1.0)
    source: Literal["prior", "fit"] = "fit"
    fitAt: int | None = None
    notes: str = Field("", max_length=500)


def _models():
    from app.db.firestore import get_db

    return get_db().collection("batteryModels")


def get_model(model_id: str) -> dict[str, Any]:
    snap = _models().document(model_id).get()
    if not snap.exists:
        return {**PRIORS, "id": model_id, "source": "prior", "fitAt": None}
    return {**PRIORS, "fitAt": None, **(snap.to_dict() or {}), "id": model_id}


def put_model(model_id: str, m: BatteryModel, uid: str) -> dict[str, Any]:
    doc = m.model_dump()
    doc["updatedAt"] = int(time.time())
    doc["updatedBy"] = uid
    _models().document(model_id).set(doc)
    return {**doc, "id": model_id}


def model_mah(sample: dict[str, Any], m: dict[str, Any]) -> float | None:
    """§3 formula over one stored sample; None if it carried no `bs`."""
    if not sample.get("hasBs"):
        return None
    aw = sample.get("awakeS") or {}
    ms = sample.get("modemS") or {}
    rf = sample.get("refresh") or {}
    cur = m["awake_mA"]
    off = ms.get("off", 0)
    search = ms.get("search", 0)
    gnss = ms.get("gnss", 0)
    idle = max(0, sample.get("dtS", 0) - off - search - gnss)
    ma_s = (
        m["sleep_mA"] * sample.get("sleepS", 0)
        + sum(cur[c] * aw.get(c, 0) for c in CAUSES)
        + m["rail_mA"] * sample.get("railS", 0)
        + m["modemIdle_mA"] * idle
        + m["modemOff_mA"] * off
        + m["modemSearch_mA"] * search
        + m["gnss_mA"] * gnss
    )
    return (
        ma_s / 3600
        + m["radioEvent_mAh"] * sample.get("radioEvents", 0)
        + m["connect_mAh"] * sample.get("connects", 0)
        + m["fullRefresh_mAh"] * (rf.get("full", 0) + rf.get("upgraded", 0))
        + m["partialRefresh_mAh"] * rf.get("partial", 0)
    )

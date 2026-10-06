"""battfit on a synthetic discharge: currents = priors x 1.2."""

from __future__ import annotations

import csv
import random
import sys
from itertools import pairwise
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import battfit

TRUE_SCALE = 1.2
UNPLUG = 1_800_000_000
Q = 2500 * 0.95


def _mv(soc_pct: float) -> float:
    pts = sorted((s, v) for v, s in battfit.OCV)
    for (s0, v0), (s1, v1) in pairwise(pts):
        if s0 <= soc_pct <= s1:
            return v0 + (v1 - v0) * (soc_pct - s0) / (s1 - s0)
    return pts[-1][1] if soc_pct > 100 else pts[0][1]


def _synth(path: Path) -> None:
    pri = battfit.load_priors()
    rng = random.Random(42)
    rows = []
    for h in range(30 * 24):
        ui = (h % 24) == 18
        awake_timer = 70
        ui_s = 1800 if ui else 0
        rows.append(
            {
                "ts": UNPLUG + (h + 1) * 3600, "hasBs": "true", "dtS": 3600,
                "sleepS": 3600 - awake_timer - ui_s, "aw_timer": awake_timer, "aw_ui": ui_s,
                "railS": ui_s, "radioEvents": 13, "connects": 1 if ui else 0,
                "rf_partial": 40 if ui else 0, "rf_full": 1 if ui else 0,
            }
        )  # fmt: skip
    cur = [TRUE_SCALE * x for x in (pri["awake_mA"][c] for c in battfit.CAUSES)]
    cum = 0.0
    kept = []
    # true per-sample mAh, then stop when the cell is empty (cum >= Q): a full discharge.
    for r in rows:
        e = {k: float(v) if v not in ("true",) else 0 for k, v in r.items()}
        mah = TRUE_SCALE * (
            pri["sleep_mA"] * e["sleepS"] / 3600
            + pri["modemIdle_mA"] * e["dtS"] / 3600
            + pri["rail_mA"] * e["railS"] / 3600
            + pri["radioEvent_mAh"] * e["radioEvents"]
            + pri["connect_mAh"] * e["connects"]
            + pri["fullRefresh_mAh"] * e["rf_full"]
            + pri["partialRefresh_mAh"] * e["rf_partial"]
        ) + (cur[0] * e["aw_timer"] + cur[3] * e["aw_ui"]) / 3600
        if cum + mah > Q:
            break
        cum += mah
        r["battMv"] = round(_mv(100 * (1 - cum / Q)) + rng.uniform(-5, 5))
        kept.append(r)
    assert len(kept) > 15 * 24  # the cell empties after ~19 of the 30 days
    cols = ["ts", "hasBs", "battMv", "dtS", "sleepS", "aw_timer", "aw_ui", "railS", "radioEvents",
            "connects", "rf_full", "rf_partial", "rf_upgraded", "md_off", "md_search", "md_gnss"]  # fmt: skip
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, restval="")
        w.writeheader()
        w.writerows(kept)


def test_fit_recovers_scale(tmp_path):
    p = tmp_path / "s.csv"
    _synth(p)
    priors = battfit.load_priors()
    res = battfit.fit(battfit.read_samples(str(p), UNPLUG), priors, 2500, 0.95)
    print(f"fitted k = {res['k']:.4f}, total = {res['totalMah']:.1f} (Q={Q})")
    assert 1.14 <= res["k"] <= 1.26
    assert abs(res["totalMah"] - Q) / Q < 0.01
    body = battfit.to_body(res, priors, 2500, 0.95)
    assert body["source"] == "fit" and len(body["notes"]) <= 500
    assert set(body["awake_mA"]) == set(battfit.CAUSES)


def test_usb_and_pre_unplug_samples_dropped(tmp_path):
    p = tmp_path / "s.csv"
    p.write_text(
        "ts,hasBs,battMv,dtS\n"
        f"{UNPLUG - 1},true,3900,3600\n{UNPLUG + 1},true,4500,3600\n"
        f"{UNPLUG + 2},false,3900,3600\n{UNPLUG + 3},true,3900,3600\n"
    )
    assert len(battfit.read_samples(str(p), UNPLUG)) == 1

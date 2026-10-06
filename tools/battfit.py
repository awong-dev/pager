"""Fit the battery current model from one full discharge -- docs/BATTERY_STATS_DESIGN.md §4.

    battfit.py samples.csv --unplug <epoch> [--capacity 2500 --usable 0.95 --model walter-6092-lipo2500]

`samples.csv` is the web Battery card's CSV export. Pure stdlib. The priors are
read from relay/app/battmodel.py (parsed with `ast`, so pydantic is not needed).
Prints the JSON body for `PUT /api/admin/battery-models/<model>` and a curl
line with a `$TOKEN` placeholder; it never sends anything.
"""

from __future__ import annotations

import argparse
import ast
import csv
import json
import math
import sys
import time
from itertools import pairwise
from pathlib import Path
from typing import Any

USB_MV = 4300
BIN_S = 6 * 3600

OCV = [
    (4200, 100), (4150, 95), (4110, 90), (4080, 85), (4020, 80), (3980, 75), (3950, 70),
    (3910, 65), (3870, 60), (3850, 55), (3840, 50), (3820, 45), (3800, 40), (3790, 35),
    (3770, 30), (3750, 25), (3730, 20), (3710, 15), (3690, 10), (3610, 5), (3270, 0),
]  # fmt: skip

CAUSES = ("timer", "attn", "hot", "ui", "modem", "fetch")
# (term name, prior path in PRIORS). 6 awake currents, 6 other currents, 4 mAh-per-event terms.
TERMS = (
    [(f"awake_mA.{c}", ("awake_mA", c)) for c in CAUSES]
    + [(n, (n,)) for n in ("sleep_mA", "rail_mA", "modemIdle_mA", "modemOff_mA", "modemSearch_mA", "gnss_mA")]
    + [(n, (n,)) for n in ("radioEvent_mAh", "connect_mAh", "fullRefresh_mAh", "partialRefresh_mAh")]
)  # fmt: skip
N = len(TERMS)


def load_priors() -> dict[str, Any]:
    src = Path(__file__).resolve().parent.parent / "relay" / "app" / "battmodel.py"
    for node in ast.parse(src.read_text()).body:
        target = node.target if isinstance(node, ast.AnnAssign) else None
        if isinstance(node, ast.Assign):
            target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == "PRIORS":
            return ast.literal_eval(node.value)  # type: ignore[arg-type]
    raise SystemExit("PRIORS not found in battmodel.py")


def soc(mv: float) -> float:
    """Generic LiPo OCV table, linear between points, clamped to 0..100."""
    if mv >= OCV[0][0]:
        return 100.0
    if mv <= OCV[-1][0]:
        return 0.0
    for (v1, s1), (v0, s0) in pairwise(OCV):
        if v0 <= mv <= v1:
            return s0 + (s1 - s0) * (mv - v0) / (v1 - v0)
    return 0.0


def _num(row: dict[str, str], key: str) -> float:
    v = row.get(key, "")
    return float(v) if v not in ("", None) else 0.0


def exposure(row: dict[str, str]) -> list[float]:
    """One sample's exposure vector: hours for mA terms, counts for mAh terms."""
    off, search, gnss = _num(row, "md_off"), _num(row, "md_search"), _num(row, "md_gnss")
    idle = max(0.0, _num(row, "dtS") - off - search - gnss)
    return [
        *(_num(row, f"aw_{c}") / 3600 for c in CAUSES),
        _num(row, "sleepS") / 3600,
        _num(row, "railS") / 3600,
        idle / 3600,
        off / 3600,
        search / 3600,
        gnss / 3600,
        _num(row, "radioEvents"),
        _num(row, "connects"),
        _num(row, "rf_full") + _num(row, "rf_upgraded"),
        _num(row, "rf_partial"),
    ]


def read_samples(path: str, unplug: int) -> list[dict[str, str]]:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    keep = [
        r
        for r in rows
        if r.get("ts") and int(float(r["ts"])) >= unplug
        and r.get("hasBs", "").lower() in ("1", "true")
        and r.get("battMv", "") != ""
        and float(r["battMv"]) < USB_MV
    ]
    keep.sort(key=lambda r: float(r["ts"]))
    return keep


def make_bins(samples: list[dict[str, str]]) -> list[dict[str, Any]]:
    """6 h bins. Each sample's window ends at its `ts` and the voltage is read at `ts`,
    so a bin's ΔSoC runs from the previous bin's last voltage to this bin's last voltage
    (the first sample only supplies the starting voltage)."""
    if len(samples) < 2:
        raise SystemExit("need at least 2 usable samples")
    t0 = float(samples[0]["ts"])
    groups: dict[int, list[dict[str, str]]] = {}
    for r in samples[1:]:
        groups.setdefault(int((float(r["ts"]) - t0) // BIN_S), []).append(r)
    bins = []
    prev_mv = float(samples[0]["battMv"])
    for k in sorted(groups):
        g = groups[k]
        end_mv = float(g[-1]["battMv"])
        a = [0.0] * N
        for r in g:
            for j, x in enumerate(exposure(r)):
                a[j] += x
        plateau = 3700 <= prev_mv <= 3900 and 3700 <= end_mv <= 3900
        bins.append({"a": a, "dsoc": soc(prev_mv) - soc(end_mv), "w": 0.25 if plateau else 1.0})
        prev_mv = end_mv
    return bins


def invert(m: list[list[float]]) -> list[list[float]]:
    """Gauss-Jordan inverse with partial pivoting."""
    n = len(m)
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(m)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-300:
            raise SystemExit("singular normal matrix")
        a[c], a[p] = a[p], a[c]
        pv = a[c][c]
        a[c] = [x / pv for x in a[c]]
        for r in range(n):
            if r != c and a[r][c] != 0.0:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c], strict=True)]
    return [row[n:] for row in a]


def solve(
    bins: list[dict[str, Any]], theta0: list[float], q: float, fixed: dict[int, float] | None = None
) -> tuple[list[float], list[float]]:
    """Ridge solve; `fixed` terms are held at the given values (removed from the system).
    Returns (theta, posterior sigma) over all N terms (sigma 0 for fixed terms)."""
    fixed = fixed or {}
    free = [j for j in range(N) if j not in fixed]
    s2 = (0.05 * q) ** 2
    n = len(free)
    h = [[0.0] * n for _ in range(n)]
    g = [0.0] * n
    tot = [sum(b["a"][j] for b in bins) for j in range(N)]
    fixed_tot = sum(tot[j] * v for j, v in fixed.items())
    rows = []  # (a_free, y, weight): bin rows and the total row
    for b in bins:
        y = b["dsoc"] / 100.0 * q - sum(b["a"][j] * v for j, v in fixed.items())
        rows.append(([b["a"][j] for j in free], y, b["w"] / s2))
    rows.append(([tot[j] for j in free], q - fixed_tot, 1e4 / s2))
    for a, y, w in rows:
        for i in range(n):
            if a[i] == 0.0:
                continue
            g[i] += w * a[i] * y
            for k in range(n):
                h[i][k] += w * a[i] * a[k]
    for i, j in enumerate(free):
        p = 1.0 / (0.5 * theta0[j]) ** 2
        h[i][i] += p
        g[i] += p * theta0[j]
    hinv = invert(h)
    sol = [sum(hinv[i][k] * g[k] for k in range(n)) for i in range(n)]
    theta = [0.0] * N
    sigma = [0.0] * N
    for j, v in fixed.items():
        theta[j] = v
    for i, j in enumerate(free):
        theta[j] = sol[i]
        sigma[j] = math.sqrt(max(hinv[i][i], 0.0))
    return theta, sigma


def fit(
    samples: list[dict[str, str]], priors: dict[str, Any], capacity: float, usable: float
) -> dict[str, Any]:
    q = capacity * usable
    bins = make_bins(samples)
    theta0 = [
        float(priors[p[0]][p[1]] if len(p) == 2 else priors[p[0]]) for _, p in TERMS
    ]
    tot = [sum(b["a"][j] for b in bins) for j in range(N)]
    prior_mah = [tot[j] * theta0[j] for j in range(N)]
    sum_prior = sum(prior_mah)
    if sum_prior <= 0:
        raise SystemExit("no exposure in the data")
    k = q / sum_prior
    theta, sigma = solve(bins, theta0, q)
    weak = {
        j
        for j in range(N)
        if theta[j] <= 0
        or sigma[j] / theta[j] > 0.25
        or prior_mah[j] / sum_prior < 0.02
    }
    if weak:
        theta, sigma = solve(bins, theta0, q, {j: k * theta0[j] for j in weak})
    resid = [
        (sum(b["a"][j] * theta[j] for j in range(N)) / q - b["dsoc"] / 100.0) * 100.0
        for b in bins
    ]
    rms = math.sqrt(sum(r * r for r in resid) / len(resid))
    total = sum(tot[j] * theta[j] for j in range(N))
    terms = {
        name: {
            "value": theta[j],
            "sigma": sigma[j],
            "share": prior_mah[j] / sum_prior,
            "scaled": j in weak,
        }
        for j, (name, _) in enumerate(TERMS)
    }
    return {"k": k, "rmsPctSoc": rms, "totalMah": total, "q": q, "bins": len(bins), "terms": terms}


def to_body(result: dict[str, Any], priors: dict[str, Any], capacity: float, usable: float) -> dict:
    t = result["terms"]
    body: dict[str, Any] = {
        "awake_mA": {c: round(t[f"awake_mA.{c}"]["value"], 4) for c in CAUSES},
    }
    for name, path in TERMS:
        if len(path) == 1:
            body[name] = round(t[name]["value"], 6)
    body["capacityMah"] = int(capacity)
    body["usableFrac"] = usable
    body["source"] = "fit"
    body["fitAt"] = int(time.time())
    parts = [f"k={result['k']:.3f} rms={result['rmsPctSoc']:.2f}%SoC bins={result['bins']}"]
    for name, v in t.items():
        short = name.replace("awake_mA.", "aw_").replace("_mAh", "").replace("_mA", "")
        parts.append(f"{short}={v['value']:.4g}/{v['sigma']:.2g}/{v['share'] * 100:.0f}%" + ("*" if v["scaled"] else ""))
    body["notes"] = "; ".join(parts)[:500]
    return body


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--unplug", type=int, required=True, help="epoch seconds when the charger was unplugged")
    ap.add_argument("--capacity", type=float, default=2500)
    ap.add_argument("--usable", type=float, default=0.95)
    ap.add_argument("--model", default="walter-6092-lipo2500")
    args = ap.parse_args(argv)
    priors = load_priors()
    samples = read_samples(args.csv, args.unplug)
    result = fit(samples, priors, args.capacity, args.usable)
    body = to_body(result, priors, args.capacity, args.usable)
    print(json.dumps(body, indent=2))
    print(
        f"\n# k={result['k']:.3f} rms={result['rmsPctSoc']:.2f}% SoC "
        f"total={result['totalMah']:.0f} mAh (target {result['q']:.0f})",
        file=sys.stderr,
    )
    print(
        f"curl -X PUT -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' "
        f"-d @body.json https://<relay>/api/admin/battery-models/{args.model}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

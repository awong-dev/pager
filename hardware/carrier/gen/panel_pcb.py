#!/usr/bin/env python3
"""Panel board PCB: 40 x 20 mm strip under the glass at the FPC end.
Run with KiCad's python: ... panel_pcb.py [--route]"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import pcbnew  # noqa: E402

from build import build_panel  # noqa: E402
from carrier_pcb import groups_by_tag  # noqa: E402
from pcb import Board  # noqa: E402

W, H = 40.0, 20.0


def main(route=False):
    prj, _ = build_panel(cli=False)
    d = os.path.normpath(os.path.join(HERE, "..", "panel"))
    b = Board(prj, d, "pager-panel", thickness=1.0)
    b.load_all()
    b.outline_polyline([(0, 0), (W, 0), (W, H), (0, H)], radius=1.0)
    P = {
        "J1": (20.0, 16.3, 0),      # ZIF, FPC enters from the bottom edge (the glass side)
        "J2": (8.0, 3.3, 180),      # SH8 to the carrier, opening at the top edge
        "L1": (18.0, 4.5, 0),
        "Q5": (23.0, 4.0, 90),
        "D3": (28.5, 2.5, 0), "D2": (28.5, 4.8, 0), "D1": (28.5, 7.1, 0),
    }
    for ref, (x, y, r) in P.items():
        b.place(ref, x, y, r)
    groups = groups_by_tag(prj)
    for a, refs in groups.items():
        refs[:] = [r for r in refs if r in b.fps and r not in P]
    rects = {
        "J1": (1.0, 7.2, 15.5, 12.8, 90),
        "BOOST": (31.5, 1.0, 39.5, 12.8, 90),
    }
    for a, refs in groups.items():
        x0, y0, x1, y1, rot = rects[a]
        b.pack(sorted(refs), x0, y0, x1, y1, rot=rot)
    b.text("pager-panel rev A", 20, 9.6, pcbnew.F_SilkS, 0.8)
    b.text("FPC from glass", 30, 19.1, pcbnew.F_SilkS, 0.8)
    if route:
        b.route(passes=20)
    outline = [(0, 0), (W, 0), (W, H), (0, H)]
    b.zone("GND", outline, pcbnew.F_Cu, "GND top")
    b.zone("GND", outline, pcbnew.B_Cu, "GND bottom")
    b.fill()
    print("saved", b.save())
    return b


if __name__ == "__main__":
    main(route="--route" in sys.argv)

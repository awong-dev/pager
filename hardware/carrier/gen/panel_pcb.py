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
    b.edge_keepout([(0, 0), (W, 0), (W, H), (0, H)])
    P = {
        "J1": (20.0, 17.6, 180),    # display ZIF, contacts toward the bottom edge: the FPC enters from there (glass side)
        "J3": (33.0, 17.6, 180),    # FL-A: front-light ZIF for the FL03 corner tail (pin-1 side of J1)
        "J4": (9.2, 17.6, 180),     # FL-B: front-light ZIF for the Orient parallel tail, 10.8 mm from J1 on its pin-24 side
        "J2": (8.0, 3.3, 180),      # SH10 to the carrier, opening at the top edge
        # the four 0R front-light polarity links in a row above J1's courtyard, between the silk title and the boost cluster
        "R1": (26.0, 12.0, 90), "R2": (27.6, 12.0, 90), "R3": (29.2, 12.0, 90), "R5": (30.8, 12.0, 90),
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
        "FL": (26.0, 10.5, 31.3, 13.4, 90),    # (unused: R1/R2/R3/R5 are placed explicitly above)
    }
    for a, refs in groups.items():
        x0, y0, x1, y1, rot = rects[a]
        b.pack(sorted(refs), x0, y0, x1, y1, rot=rot)
    b.text("pager-panel rev A", 20, 9.6, pcbnew.F_SilkS, 0.8)
    b.text("display FPC", 20.0, 12.9, pcbnew.F_SilkS, 0.8)
    b.text("FL-A", 37.6, 14.6, pcbnew.F_SilkS, 0.8, rot=90)
    b.text("FL-B", 4.0, 15.6, pcbnew.F_SilkS, 0.8, rot=90)
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

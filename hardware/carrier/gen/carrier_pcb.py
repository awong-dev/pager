#!/usr/bin/env python3
"""Carrier PCB: 85 x 52 mm, Walter crosswise under the left end, battery under
the rest. Run with KiCad's python:

  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 carrier_pcb.py [--route]
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import pcbnew  # noqa: E402

from build import build_carrier, build_panel  # noqa: E402
from pcb import Board  # noqa: E402

W, H = 85.0, 52.0
NOTCH = (2.5, 14.5, 6.0)  # x0, x1, depth at the top edge for the antenna pigtails
WALTER_X, WALTER_Y = 13.5, 24.73  # header centre; module spans x 1.1..25.9, y -1.5..53.5
UFL = [(WALTER_X - 3.0, WALTER_Y - 21.93), (WALTER_X - 7.3, WALTER_Y - 21.93)]  # LTE, GNSS jacks (mirrored X)
GLASS = (0.5, 8.0, 79.5, 44.7)  # 2.9" glass footprint on the carrier top (79 x 36.7)
PANEL_ZONE = (62.0, 8.0, 79.5, 44.7)  # the panel board lies here, under the glass


def groups_by_tag(prj):
    """Passives carry a .group tag set in carrier.py (grp())."""
    groups = {}
    seen = set()
    for sh in prj.sheets:
        for i in sh.instances:
            g = getattr(i, "group", None)
            if g and i.ref not in seen:
                groups.setdefault(g, []).append(i.ref)
                seen.add(i.ref)
    return groups


def main(route=False):
    prj, _ = build_carrier(cli=False)
    d = os.path.normpath(os.path.join(HERE, ".."))
    b = Board(prj, d, "pager-carrier", thickness=1.0)
    b.load_all()

    # ---- outline ---------------------------------------------------------
    nx0, nx1, nd = NOTCH
    pts = [(0, 0), (nx0, 0), (nx0, nd), (nx1, nd), (nx1, 0), (W, 0), (W, H), (0, H)]
    b.outline_polyline(pts, radius=2.0)

    # ---- fixed placements ------------------------------------------------
    P = {
        "M1": (WALTER_X, WALTER_Y, 0),
        "U5": (13.5, 12.5, 0), "U6": (9.8, 23.7, 0), "Q2": (17.0, 23.7, 0), "JP1": (25.4, 44.0, 90), "F1": (25.3, 34.0, 90),
        "U1": (13.5, 31.0, 0),
        "Q1": (9.0, 44.0, 0),
        "J1": (34.0, 5.0, 0), "D4": (30.5, 10.3, 0),
        "SW1": (47.5, 5.0, 0),
        "J3": (60.0, 4.3, 180), "J2": (75.0, 5.3, 180),
        "SW2": (72.6, 47.8, 0),
        "J4": (34.5, 47.0, 0), "J6": (44.5, 48.8, 0), "J7": (50.8, 48.8, 0), "D5": (25.5, 5.6, 90),
        "U3": (33.0, 16.0, 0), "L2": (37.5, 16.0, 0),
        "U4": (32.0, 26.0, 0),
        "U7": (52.0, 20.0, 0),
        "U10": (45.0, 27.0, 0),
        "Q4": (46.0, 11.5, 0),
        "U9": (56.0, 11.5, 0),
        "H1": (17.0, 2.5, 0), "H2": (82.8, 2.2, 0), "H3": (82.8, 49.8, 0), "H4": (2.5, 49.5, 0),
    }
    for ref, (x, y, r) in P.items():
        b.place(ref, x, y, r)
    # test pads along the bottom strip
    for i in range(14):  # two rows of seven in the bottom strip
        b.place("TP%d" % (i + 1), 55.1 + (i % 7) * 2.0, 48.9 if i < 7 else 50.9, 0)

    # ---- passives packed next to their anchors --------------------------
    groups = groups_by_tag(prj)
    for a, refs in groups.items():
        refs[:] = [r for r in refs if r in b.fps and r not in P and not r.startswith("TP")]
    # bulk caps near Walter VIN (pin 28 at x 4.6, y 8.2)
    walter_caps = sorted(groups.pop("M1", []))
    rects = {
        "U5": (6.6, 15.3, 20.6, 20.5, 90),
        "U6": (6.6, 26.0, 11.8, 29.0, 90), "Q2": (14.5, 26.0, 20.6, 29.0, 90),
        "U1": (6.6, 33.6, 20.6, 41.5, 90),
        "Q1": (11.5, 42.6, 20.6, 47.0, 90),
        "J1": (27.5, 13.0, 39.5, 15.4, 90), "VBUSDET": (33.0, 9.0, 40.0, 11.6, 90),
        "U3": (27.5, 18.5, 46.0, 22.5, 90),
        "U4": (27.5, 28.5, 40.0, 31.5, 90), "J3": (36.0, 32.5, 44.0, 35.0, 90),
        "U7": (47.0, 22.5, 60.0, 25.0, 90),
        "U10": (42.0, 29.5, 48.0, 31.5, 90),
        "Q4": (41.0, 13.5, 52.5, 16.5, 90),
        "SW2": (76.9, 45.5, 80.0, 49.2, 0),
        "U9": (53.0, 14.0, 61.0, 17.5, 90),
        "J4": (27.5, 38.3, 34.0, 42.0, 90),
        "D5": (24.2, 0.7, 27.8, 3.8, 90),
        "J2": (27.5, 33.0, 33.5, 36.0, 90), "F1": (27.5, 33.0, 33.5, 36.0, 90),
    }
    # Walter bulk: 2 x 100uF 0805 + 10uF in a row just below the notch, next to pin 28 (4.6, 8.2)
    b.pack(walter_caps, 6.6, 7.6, 20.0, 9.9, rot=0)
    for a, refs in groups.items():
        if not refs:
            continue
        if a not in rects:
            print("unplaced group:", a, refs)
            continue
        x0, y0, x1, y1, rot = rects[a]
        b.pack(sorted(refs), x0, y0, x1, y1, rot=rot)

    # ---- keep-outs, zones, notes ---------------------------------------
    for cx, cy in UFL:
        b.keepout(Board.circle_pts(cx, cy, 3.0), fps=False, name="u.FL keep-out")
    b.edge_keepout(pts)
    gx0, gy0, gx1, gy1 = GLASS
    for a, c in (((gx0, gy0), (gx1, gy0)), ((gx1, gy0), (gx1, gy1)), ((gx1, gy1), (gx0, gy1)), ((gx0, gy1), (gx0, gy0))):
        b.seg(a, c, layer=pcbnew.Cmts_User, w=0.15)
    px0, py0, px1, py1 = PANEL_ZONE
    for a, c in (((px0, py0), (px1, py0)), ((px1, py0), (px1, py1)), ((px1, py1), (px0, py1)), ((px0, py1), (px0, py0))):
        b.seg(a, c, layer=pcbnew.Cmts_User, w=0.15)
    b.text("glass 79 x 36.7 (GDEY029T94)", 20, 43.5, pcbnew.Cmts_User, 1.0)
    b.text("panel board here", 70.5, 26, pcbnew.Cmts_User, 1.0)
    b.text("antenna pigtails", 8.5, 7.2, pcbnew.Cmts_User, 0.8)
    b.text("SIM loads from this edge (Walter beneath)", 13.5, 51.2, pcbnew.Cmts_User, 0.8)
    b.text("pager-carrier rev A", 44, 46.0, pcbnew.F_SilkS, 1.0)
    b.text("accel X ->", 52.0, 26.2, pcbnew.F_SilkS, 0.8)
    b.text("LIGHT", 81.3, 46.6, pcbnew.F_SilkS, 0.8)
    b.text("WALTER UNDERNEATH, header side up", 13.5, 6.9, pcbnew.B_SilkS, 0.8)

    if route:
        b.route(passes=int(os.environ.get("PASSES", "30")))
    outline = [(0, 0), (W, 0), (W, H), (0, H)]
    b.zone("GND", outline, pcbnew.F_Cu, "GND top")
    b.zone("GND", outline, pcbnew.B_Cu, "GND bottom")
    b.fill()
    path = b.save()
    print("saved", path)
    return b


if __name__ == "__main__":
    main(route="--route" in sys.argv)

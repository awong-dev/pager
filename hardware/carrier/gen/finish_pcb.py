#!/usr/bin/env python3
"""Post-process a generated board without rerouting: sync Datasheet /
Description fields from the schematic model, set zone island removal,
refill zones, save. Usage: finish_pcb.py carrier|panel"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pcbnew  # noqa: E402
from build import build_carrier, build_panel  # noqa: E402
from pcb import apply_design_settings  # noqa: E402

which = sys.argv[1]
if which == "carrier":
    prj, _ = build_carrier(cli=False)
    path = os.path.normpath(os.path.join(HERE, "..", "pager-carrier.kicad_pcb"))
else:
    prj, _ = build_panel(cli=False)
    path = os.path.normpath(os.path.join(HERE, "..", "panel", "pager-panel.kicad_pcb"))
board = pcbnew.LoadBoard(path)
apply_design_settings(board, 1.0)
for fp in board.GetFootprints():
    ref = fp.GetReference()
    if ref not in prj.components:
        continue
    inst = prj.components[ref][0]
    for k in ("Datasheet", "Description"):
        fp.SetField(k, inst.fields.get(k, ""))
    for f in fp.GetFields():
        if f.GetName() in ("Datasheet", "Description", "MPN", "LCSC", "DNP", "Note"):
            f.SetVisible(False)
for z in board.Zones():
    if not z.GetIsRuleArea():
        try:
            z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        except AttributeError:
            pass
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(path, board)
print("finished", path)

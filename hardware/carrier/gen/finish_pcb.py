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
# Freerouting's optimizer leaves vias that only F.Cu (or only B.Cu) tracks touch;
# drop any via whose other layer has no track end and no zone of its net.
tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
zone_layers = {}
for z in board.Zones():
    if not z.GetIsRuleArea():
        for l in (pcbnew.F_Cu, pcbnew.B_Cu):
            if z.IsOnLayer(l):
                zone_layers.setdefault(z.GetNetname(), set()).add(l)
dropped = 0
for v in [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]:
    p = v.GetPosition()
    touched = set(t.GetLayer() for t in tracks if t.GetStart() == p or t.GetEnd() == p)
    for l in (pcbnew.F_Cu, pcbnew.B_Cu):
        if l in zone_layers.get(v.GetNetname(), ()):
            touched.add(l)
    if len(touched) == 1:
        board.Remove(v)
        dropped += 1
print("dangling vias removed:", dropped)
for z in board.Zones():
    if not z.GetIsRuleArea():
        try:
            z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        except AttributeError:
            pass
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(path, board)
print("finished", path)

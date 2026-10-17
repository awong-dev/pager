#!/usr/bin/env python3
"""Generate the pager-carrier and pager-panel KiCad projects and run the
kicad-cli exports (ERC, PDF, BOM, netlist).

    python3 build.py            # writes ../ and ../panel/
    python3 build.py --no-cli   # files only
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from sch import Project  # noqa: E402
from customlib import write_lib  # noqa: E402
from symbols import all_symbols  # noqa: E402
from footprints import walter_module, bq25185_wson10, tps62840_dlc, usb4235, fh34srj, write_pretty  # noqa: E402
import carrier  # noqa: E402
import panel  # noqa: E402

KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
DATE = "2026-10-17"

# GCT USB4235 drawing rev A (21 Aug 2024): origin = connector centre, X along
# the long axis, +Y toward the A row. 16 SMD pads 0.30 x 0.87 on 0.5 pitch,
# shell stake slots at (+/-5.0, 0) 0.6 x 0.85 drill with 1.1 x 1.35 copper,
# NPTH peg holes 0.6 mm at (+/-3.75, 0). Height 6.50 mm. Board 1.0 +/- 0.1 mm.
USB4235_SPEC = {
    "height": 6.5,
    "pads": [(n, x, 0.87, 0.30, 0.87) for n, x in (
        ("A1", -2.75), ("A4", -1.25), ("A5", -0.75), ("A6", -0.25), ("A7", 0.25), ("A8", 0.75),
        ("A9", 1.25), ("A12", 2.75))] +
            [(n, x, -0.87, 0.30, 0.87) for n, x in (
                ("B12", -2.75), ("B9", -1.25), ("B8", -0.75), ("B7", -0.25), ("B6", 0.25), ("B5", 0.75),
                ("B4", 1.25), ("B1", 2.75))],
    "holes": [(-3.75, 0, 0.6, None), (3.75, 0, 0.6, None)],
    "body": (-4.47, -1.58, 4.47, 1.58),
    "courtyard": (-5.85, -1.95, 5.85, 1.95),
}


def usb4235_footprint():
    name, node = usb4235(USB4235_SPEC)
    # plated shell slots (oval), net SHIELD -> pad name "SH"
    for x in (-5.0, 5.0):
        node.insert(-1, ["pad", '"SH"', "thru_hole", "oval", ["at", x, 0], ["size", 1.1, 1.35],
                         ["drill", "oval", 0.6, 0.85], ["layers", '"*.Cu"', '"*.Mask"'],
                         ["remove_unused_layers", "no"]])
    return name, node


def fix_quotes(node):
    """Our insert above used pre-quoted strings; convert them to Quoted."""
    from sexp import Quoted
    out = []
    for a in node:
        if isinstance(a, list):
            out.append(fix_quotes(a))
        elif isinstance(a, str) and len(a) >= 2 and a[0] == '"' and a[-1] == '"' and not isinstance(a, Quoted):
            out.append(Quoted(a[1:-1]))
        else:
            out.append(a)
    return out


def write_project_files(d, name, sheets_root):
    pro = {
        "board": {"design_settings": {"defaults": {}, "diff_pair_dimensions": [], "drc_exclusions": [],
                                      "rules": {}, "track_widths": [], "via_dimensions": []}},
        "boards": [], "cvpcb": {"equivalence_files": []},
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": name + ".kicad_pro", "version": 3},
        "net_settings": {"classes": [{"bus_width": 12, "clearance": 0.15, "diff_pair_gap": 0.25,
                                      "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
                                      "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": "Default",
                                      "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
                                      "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3, "wire_width": 6}],
                         "meta": {"version": 4}, "net_colors": None, "netclass_assignments": None,
                         "netclass_patterns": []},
        "pcbnew": {"last_paths": {"gencad": "", "idf": "", "netlist": "", "plot": "", "pos_files": "",
                                  "specctra_dsn": "", "step": "", "svg": "", "vrml": ""}, "page_layout_descr_file": ""},
        "schematic": {"drawing": {"label_size_ratio": 0.375, "pin_symbol_size": 25.0, "text_offset_ratio": 0.15},
                      "legacy_lib_dir": "", "legacy_lib_list": []},
        "sheets": [], "text_variables": {},
    }
    with open(os.path.join(d, name + ".kicad_pro"), "w") as f:
        json.dump(pro, f, indent=2)
    with open(os.path.join(d, "sym-lib-table"), "w") as f:
        f.write('(sym_lib_table\n  (version 7)\n'
                '  (lib (name "pager-carrier")(type "KiCad")(uri "%s")(options "")(descr "pager carrier custom symbols"))\n)\n' % sheets_root)
    with open(os.path.join(d, "fp-lib-table"), "w") as f:
        f.write('(fp_lib_table\n  (version 7)\n'
                '  (lib (name "pager-carrier")(type "KiCad")(uri "%s")(options "")(descr "pager carrier custom footprints"))\n)\n'
                % sheets_root.replace(".kicad_sym", ".pretty"))


def run(cmd):
    print("+", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(r.stdout[-2000:])
    if r.returncode != 0:
        sys.stdout.write(r.stderr[-2000:])
    return r.returncode


def exports(d, name, root_sch):
    out = os.path.join(d, "out")
    os.makedirs(out, exist_ok=True)
    rc = run([KICAD_CLI, "sch", "erc", "--severity-all", "--format", "report", "-o",
              os.path.join(out, name + "-erc.txt"), root_sch])
    run([KICAD_CLI, "sch", "export", "pdf", "-o", os.path.join(out, name + "-schematic.pdf"), root_sch])
    run([KICAD_CLI, "sch", "export", "bom", "--fields", "Reference,Value,Footprint,MPN,LCSC,DNP,${QUANTITY}",
         "--labels", "Reference,Value,Footprint,MPN,LCSC,DNP,Qty", "--group-by", "Value,Footprint,MPN,DNP",
         "--ref-range-delimiter", "", "-o", os.path.join(out, name + "-bom.csv"), root_sch])
    run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadsexpr", "-o",
         os.path.join(out, name + ".net"), root_sch])
    return rc


def build_carrier(cli=True):
    d = os.path.normpath(os.path.join(HERE, ".."))
    lib = os.path.join(d, "pager-carrier.kicad_sym")
    write_lib(lib, all_symbols())
    write_pretty(os.path.join(d, "pager-carrier.pretty"),
                 [walter_module(), bq25185_wson10(), tps62840_dlc(), fix_quotes(usb4235_footprint()),
                  fh34srj(24), fh34srj(6)])
    prj = Project("pager-carrier", DATE, rev="A", company="awong-dev/pager")
    prj.custom_lib = lib
    carrier.build(prj, lib)
    for s in prj.sheets:
        s.write(os.path.join(d, s.filename))
    prj.write_root(os.path.join(d, "pager-carrier.kicad_sch"), "pager-carrier.kicad_sch")
    write_project_files(d, "pager-carrier", "${KIPRJMOD}/pager-carrier.kicad_sym")
    rc = exports(d, "pager-carrier", os.path.join(d, "pager-carrier.kicad_sch")) if cli else 0
    return prj, rc


def build_panel(cli=True):
    d = os.path.normpath(os.path.join(HERE, "..", "panel"))
    os.makedirs(d, exist_ok=True)
    lib = os.path.normpath(os.path.join(HERE, "..", "pager-carrier.kicad_sym"))
    prj = Project("pager-panel", DATE, rev="A", company="awong-dev/pager")
    prj.custom_lib = lib
    panel.build(prj, lib)
    for s in prj.sheets:
        s.write(os.path.join(d, s.filename))
    prj.write_root(os.path.join(d, "pager-panel.kicad_sch"), "pager-panel.kicad_sch")
    write_project_files(d, "pager-panel", "${KIPRJMOD}/../pager-carrier.kicad_sym")
    rc = exports(d, "pager-panel", os.path.join(d, "pager-panel.kicad_sch")) if cli else 0
    return prj, rc


if __name__ == "__main__":
    cli = "--no-cli" not in sys.argv
    p1, rc1 = build_carrier(cli)
    p2, rc2 = build_panel(cli)
    for prj in (p1, p2):
        single = sorted(n for n, m in prj.nets.items() if len(m) < 2)
        print(prj.name, "single-pin nets:", single)
    print("carrier nets:", len(p1.nets), "components:", len(p1.components))
    print("panel nets:", len(p2.nets), "components:", len(p2.components))

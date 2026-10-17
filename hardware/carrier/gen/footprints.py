"""Custom footprints for pager-carrier.pretty, built from manufacturer numbers.

Sources
- Walter: DPTechnics Walter datasheet rev 1.1 (hardware 1.6) section 6 and
  the vendor walter-solder.kicad_mod (rows 17.78 apart, 2.54 pitch, pin 1 at
  9.72 mm from the USB-C end). The carrier sits ABOVE a flipped Walter
  (header side up), so the pad pattern is the mirror image of the vendor's
  top-mount footprint: pin 1 is on the +X side as seen from the carrier top.
- BQ25185 DLH: TI drawing DLH0010A (4226298/A) land pattern: 10 pads
  0.5 x 0.2 mm on 0.4 mm pitch, columns 2.1 mm apart, thermal pad 0.9 x 1.5,
  two 0.2 mm vias at (0, +/-0.5).
- USB4235: GCT drawing, filled in by usb4235() from the numbers passed in.
"""

from sexp import dump, q

FP_VERSION = 20260206


def _hdr(name, descr, tags, attr="smd"):
    n = ["footprint", q(name), ["version", FP_VERSION], ["generator", q("pager_carrier_gen")],
         ["generator_version", q("10.0")], ["layer", q("F.Cu")], ["descr", q(descr)], ["tags", q(tags)]]
    if attr:
        n.append(["attr"] + attr.split())
    return n


def _text(kind, text, x, y, layer, size=1.0, thick=0.15, hide=False, rot=0):
    n = ["property" if kind in ("Reference", "Value") else "fp_text", q(kind) if kind in ("Reference", "Value") else kind,
         q(text), ["at", x, y, rot], ["layer", q(layer)]]
    if kind in ("Reference", "Value"):
        n = ["property", q(kind), q(text), ["at", x, y, rot], ["layer", q(layer)]]
    if hide:
        n.append(["hide", "yes"])
    n.append(["effects", ["font", ["size", size, size], ["thickness", thick]]])
    return n


def _line(x1, y1, x2, y2, layer, w=0.12):
    return ["fp_line", ["start", x1, y1], ["end", x2, y2], ["stroke", ["width", w], ["type", "solid"]],
            ["layer", q(layer)]]


def _rect(x1, y1, x2, y2, layer, w=0.12, fill=False):
    n = ["fp_rect", ["start", x1, y1], ["end", x2, y2], ["stroke", ["width", w], ["type", "solid"]]]
    n.append(["fill", "yes" if fill else "no"])
    n.append(["layer", q(layer)])
    return n


def _circle(cx, cy, r, layer, w=0.12, fill=False):
    return ["fp_circle", ["center", cx, cy], ["end", cx + r, cy], ["stroke", ["width", w], ["type", "solid"]],
            ["fill", "yes" if fill else "no"], ["layer", q(layer)]]


def _pad(num, kind, shape, x, y, sx, sy, layers, drill=None, rr=None, rot=0):
    n = ["pad", q(str(num)), kind, shape, ["at", x, y, rot], ["size", sx, sy]]
    if drill is not None:
        n.append(["drill", drill])
    n.append(["layers"] + [q(l) for l in layers])
    if kind == "thru_hole":
        n.append(["remove_unused_layers", "no"])
    if rr is not None:
        n.append(["roundrect_rratio", rr])
    return n


def _fp_text(text, x, y, layer, size=1.0, rot=0):
    return ["fp_text", "user", q(text), ["at", x, y, rot], ["layer", q(layer)],
            ["effects", ["font", ["size", size, size], ["thickness", 0.15]]]]


# ---------------------------------------------------------------------------
def walter_module():
    """Walter_Module_2x14_THT: carrier-top view of a header-side-up Walter.

    Footprint frame: origin at the header centre, +Y toward the Walter's
    antenna end (fy in the datasheet notes), X mirrored relative to the
    vendor drawing: pin 1 at (+8.89, -16.51), pin 28 at (-8.89, -16.51).
    """
    name = "Walter_Module_2x14_THT"
    n = _hdr(name, "DPTechnics Walter module, 2x14 2.54 mm header soldered from the carrier top; "
                   "module mounted header-side-up beneath the carrier (mirror of the vendor top-mount pattern). "
                   "Datasheet rev 1.1 (hw 1.6) section 6.",
             "walter dptechnics module header 2x14", attr="through_hole")
    n.append(["property", q("Reference"), q("M1"), ["at", 0, -28.5, 0], ["layer", q("F.SilkS")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    n.append(["property", q("Value"), q("Walter"), ["at", 0, 31.0, 0], ["layer", q("F.Fab")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    pitch = 2.54
    y0 = -16.51
    for i in range(14):
        y = round(y0 + i * pitch, 4)
        # pins 1-14 on +X (mirrored), 15-28 return on -X
        shape = "rect" if i == 0 else "circle"
        n.append(_pad(i + 1, "thru_hole", shape, 8.89, y, 2.0, 2.0, ["*.Cu", "*.Mask"], drill=1.02))
        n.append(_pad(28 - i, "thru_hole", "circle", -8.89, y, 2.0, 2.0, ["*.Cu", "*.Mask"], drill=1.02))
    # module outline (55 x 24.8) on B.CrtYd and F.Fab: USB-C end at y=-26.23, antenna end at +28.77
    yu, ya, hx = -26.23, 28.77, 12.4
    for layer, w in (("B.CrtYd", 0.05), ("F.Fab", 0.1)):
        n.append(_rect(-hx, yu, hx, ya, layer, w))
    # header housings (3.3 mm tall plastic, 35.56 long, 2.54 wide) on F.Fab
    for x in (8.89, -8.89):
        n.append(_rect(x - 1.27, -17.78, x + 1.27, 17.78, "F.Fab", 0.1))
    # silkscreen: pin 1 marker and module outline ends
    n.append(_circle(11.0, -16.51, 0.3, "F.SilkS", 0.3, fill=True))
    n.append(_line(-hx, yu, hx, yu, "F.SilkS", 0.15))
    n.append(_line(-hx, ya, hx, ya, "F.SilkS", 0.15))
    n.append(_line(-hx, yu, -hx, -19.0, "F.SilkS", 0.15))
    n.append(_line(hx, yu, hx, -19.0, "F.SilkS", 0.15))
    n.append(_line(-hx, ya, -hx, 19.0, "F.SilkS", 0.15))
    n.append(_line(hx, ya, hx, 19.0, "F.SilkS", 0.15))
    # User.Comments: SIM holder (bottom side of the Walter = facing the carrier), u.FL jacks, USB-C
    # datasheet X (+ toward pins 15-28) maps to footprint -X because of the mirror.
    def mx(x):
        return -x
    # nano-SIM holder body X -4.27..+5.27, Y 34.10..46.30 -> fy +7.87..+20.07; mouth at the antenna end
    n.append(_rect(mx(5.27), 7.87, mx(-4.27), 20.07, "Cmts.User", 0.12))
    n.append(_fp_text("nano-SIM (faces carrier)\ninserts from antenna end ->", mx(0.5), 14.0, "Cmts.User", 0.7))
    n.append(_fp_text("SIM keep-out: no parts on carrier underside within 8 mm of this edge", 0, 24.5, "Cmts.User", 0.7))
    # u.FL jacks (top side of the Walter = facing the tin floor), USB-C end: X +3.0 / +7.3, fy -21.93
    for x, lbl in ((3.0, "u.FL LTE"), (7.3, "u.FL GNSS")):
        n.append(_circle(mx(x), -21.93, 1.3, "Cmts.User", 0.12))
        n.append(_circle(mx(x), -21.93, 4.3, "Cmts.User", 0.1))  # 3 mm keep-out ring
    n.append(_fp_text("u.FL LTE / GNSS (tin-floor side); pigtails exit this end\n3 mm copper/part keep-out", mx(5.0), -24.3, "Cmts.User", 0.6))
    # USB-C receptacle (tin-floor side) X -4.31 at the USB-C edge
    n.append(_rect(mx(-4.31) - 4.5, yu, mx(-4.31) + 4.5, yu + 6.2, "Cmts.User", 0.12))
    n.append(_fp_text("Walter USB-C (buried)", mx(-4.31), yu + 3.1, "Cmts.User", 0.6))
    n.append(_fp_text("ANTENNA END", 0, 27.3, "Cmts.User", 0.8))
    n.append(_fp_text("USB-C END", 0, -25.0, "Cmts.User", 0.8))
    n.append(["embedded_fonts", "no"])
    return name, n


def bq25185_wson10():
    """Texas DLH0010A WSON-10 2.2x2 mm, 0.4 mm pitch, EP 0.9x1.5 (TI land pattern)."""
    name = "BQ25185_WSON-10_DLH0010A_2.2x2mm_P0.4mm_EP0.9x1.5mm"
    n = _hdr(name, "WSON-10 (TI DLH0010A), 2.2x2.0 mm body, 0.4 mm pitch, exposed pad 0.9x1.5 mm, "
                   "TI land pattern 4226298/A: pads 0.5x0.2, columns 2.1 mm apart, 2x 0.2 mm thermal vias",
             "WSON DLH0010A BQ25185")
    n.append(["property", q("Reference"), q("U1"), ["at", 0, -2.0, 0], ["layer", q("F.SilkS")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    n.append(["property", q("Value"), q("BQ25185DLHR"), ["at", 0, 2.0, 0], ["layer", q("F.Fab")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    # pins 1-5 on -X (top to bottom), 6-10 on +X (bottom to top), pin 1 top-left
    for i in range(5):
        y = round(-0.8 + i * 0.4, 4)
        n.append(_pad(i + 1, "smd", "roundrect", -1.05, y, 0.5, 0.2, ["F.Cu", "F.Paste", "F.Mask"], rr=0.25))
        n.append(_pad(10 - i, "smd", "roundrect", 1.05, y, 0.5, 0.2, ["F.Cu", "F.Paste", "F.Mask"], rr=0.25))
    # thermal pad 11 with reduced paste (stencil 0.85 x 1.38 per TI)
    n.append(["pad", q("11"), "smd", "rect", ["at", 0, 0], ["size", 0.9, 1.5],
              ["layers", q("F.Cu"), q("F.Mask")], ["solder_paste_margin_ratio", -0.06]])
    n.append(["pad", q("11"), "smd", "rect", ["at", 0, 0], ["size", 0.85, 1.38], ["layers", q("F.Paste")]])
    for y in (-0.5, 0.5):
        n.append(_pad(11, "thru_hole", "circle", 0, y, 0.45, 0.45, ["*.Cu"], drill=0.2))
    # fab outline 2.2 x 2.0 with pin-1 chamfer, silk corner marks, courtyard 3.1 x 2.5
    n.append(_line(-1.1, -0.6, -0.7, -1.0, "F.Fab", 0.1))
    n.append(_line(-0.7, -1.0, 1.1, -1.0, "F.Fab", 0.1))
    n.append(_line(1.1, -1.0, 1.1, 1.0, "F.Fab", 0.1))
    n.append(_line(1.1, 1.0, -1.1, 1.0, "F.Fab", 0.1))
    n.append(_line(-1.1, 1.0, -1.1, -0.6, "F.Fab", 0.1))
    n.append(_line(-1.45, -1.15, -1.45, -1.05, "F.SilkS", 0.12))
    n.append(_line(-1.45, -1.15, 1.45, -1.15, "F.SilkS", 0.12))
    n.append(_line(-1.45, 1.15, 1.45, 1.15, "F.SilkS", 0.12))
    n.append(_circle(-1.75, -1.0, 0.1, "F.SilkS", 0.2, fill=True))
    n.append(_rect(-1.55, -1.25, 1.55, 1.25, "F.CrtYd", 0.05))
    n.append(["embedded_fonts", "no"])
    return name, n


def tps62840_dlc():
    """TI DLC0008B VSON-HR (SOT-5X3) 1.5 x 2.0 mm, 0.5 mm pitch, no exposed pad.
    TI land pattern: 8 pads 0.6 x 0.25 mm, columns at x = +/-0.65 (SLVSEC6D)."""
    name = "TPS62840_VSON-HR-8_DLC0008B_1.5x2mm_P0.5mm"
    n = _hdr(name, "TI DLC0008B VSON-HR / SOT-5X3, 8 leads, 1.5x2.0 mm body, 0.5 mm pitch, "
                   "TI land pattern: pads 0.6x0.25 at x=+/-0.65 (TPS62840 datasheet SLVSEC6D)",
             "VSON-HR SOT-5X3 DLC0008B TPS62840")
    n.append(["property", q("Reference"), q("U3"), ["at", 0, -1.8, 0], ["layer", q("F.SilkS")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    n.append(["property", q("Value"), q("TPS62840DLCR"), ["at", 0, 1.8, 0], ["layer", q("F.Fab")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    for i in range(4):
        y = round(-0.75 + i * 0.5, 4)
        n.append(_pad(i + 1, "smd", "roundrect", -0.65, y, 0.6, 0.25, ["F.Cu", "F.Paste", "F.Mask"], rr=0.25))
        n.append(_pad(8 - i, "smd", "roundrect", 0.65, y, 0.6, 0.25, ["F.Cu", "F.Paste", "F.Mask"], rr=0.25))
    n.append(_rect(-0.75, -1.0, 0.75, 1.0, "F.Fab", 0.1))
    n.append(_line(-0.75, -0.6, -0.35, -1.0, "F.Fab", 0.1))
    n.append(_line(-1.1, -1.11, 1.1, -1.11, "F.SilkS", 0.12))
    n.append(_line(-1.1, 1.11, 1.1, 1.11, "F.SilkS", 0.12))
    n.append(_circle(-1.35, -0.75, 0.1, "F.SilkS", 0.2, fill=True))
    n.append(_rect(-1.2, -1.25, 1.2, 1.25, "F.CrtYd", 0.05))
    n.append(["embedded_fonts", "no"])
    return name, n


def usb4235(spec):
    """GCT USB4235 vertical USB-C, 16-pin USB 2.0.

    spec: dict with
      pads: list of (name, x, y, sx, sy) SMD pads
      holes: list of (x, y, drill, plated_size or None)
      body: (x1, y1, x2, y2) body outline
      height: overall height (for the description)
      courtyard: (x1, y1, x2, y2)
      cutout: optional list of (x1,y1,x2,y2) rectangles on Edge.Cuts
    """
    name = "USB_C_Receptacle_GCT_USB4235_Vertical"
    n = _hdr(name, "GCT USB4235 vertical (top-entry) USB Type-C receptacle, 16 pins USB 2.0, "
                   "built from the GCT product drawing; %s mm tall" % spec.get("height", "?"),
             "USB-C USB4235 vertical GCT", attr="smd")
    n.append(["property", q("Reference"), q("J1"), ["at", 0, spec["courtyard"][1] - 1.0, 0], ["layer", q("F.SilkS")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    n.append(["property", q("Value"), q("USB4235"), ["at", 0, spec["courtyard"][3] + 1.0, 0], ["layer", q("F.Fab")],
              ["effects", ["font", ["size", 1.0, 1.0], ["thickness", 0.15]]]])
    for pname, x, y, sx, sy in spec["pads"]:
        n.append(_pad(pname, "smd", "roundrect", x, y, sx, sy, ["F.Cu", "F.Paste", "F.Mask"], rr=0.25))
    for h in spec["holes"]:
        x, y, drill, size = h[:4]
        pname = h[4] if len(h) > 4 else ""
        if size is None:
            n.append(["pad", q(""), "np_thru_hole", "circle", ["at", x, y], ["size", drill, drill],
                      ["drill", drill], ["layers", q("*.Cu"), q("*.Mask")]])
        else:
            n.append(_pad(pname or "SH", "thru_hole", "circle" if len(h) < 6 else h[5], x, y, size, size,
                          ["*.Cu", "*.Mask"], drill=drill))
    for x1, y1, x2, y2 in spec.get("cutout", []):
        n.append(_rect(x1, y1, x2, y2, "Edge.Cuts", 0.05))
    x1, y1, x2, y2 = spec["body"]
    n.append(_rect(x1, y1, x2, y2, "F.Fab", 0.1))
    n.append(_rect(x1 - 0.1, y1 - 0.1, x2 + 0.1, y2 + 0.1, "F.SilkS", 0.12))
    n.append(_rect(*spec["courtyard"], "F.CrtYd", 0.05))
    n.append(_fp_text("plug enters from above", 0, (y1 + y2) / 2, "F.Fab", 0.6))
    n.append(["embedded_fonts", "no"])
    return name, n


def write_pretty(dirpath, fps):
    import os
    os.makedirs(dirpath, exist_ok=True)
    for name, node in fps:
        with open(os.path.join(dirpath, name + ".kicad_mod"), "w", encoding="utf-8") as f:
            f.write(dump(node))

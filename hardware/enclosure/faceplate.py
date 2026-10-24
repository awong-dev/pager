"""Kid-pager faceplate (display cassette top plate) for the Altoids tin build.

Parametric CadQuery model, first fit-check revision (22 Oct 2026).
Frame: the Fusion assembly frame used by hardware/carrier/Assembled*.step
(tin long axis = y, hinge wall = -x, z up from the tin floor). KiCad board frame
maps as kx = world_y + 2.03, ky = world_x + 101.27.

Outputs (hardware/enclosure/out/):
  faceplate.step          plate in the assembly frame (drop into Fusion)
  faceplate_print.stl     plate flipped top-face-down for the print bed
  slider_cap.step / slider_cap_print.stl   cap for the SW2 slide knob
  fl_cap.step / fl_cap_print.stl           button cap for the SW1 front-light tactile

Assumptions that must be checked against hardware (see README notes at the end):
  * display = bare GDEY029T94(-FL03) glass 79.0 x 36.7, glass top at GLASS_TOP,
    FPC leaving the +y (battery-end) short edge, centred;
  * carrier top at CARRIER_TOP, hole/part positions from pager-carrier.kicad_pcb
    (24 Oct 2026 outline: no holes, J6 USB4235, SW1 at the corner -> TS-1109S-C-H 7 mm, SW2 JS102011SAQN);
  * tin interior from "Assembled V0.2.step" (Altoids bottom lid v4).
Run:  <cadquery python> faceplate.py
"""
import cadquery as cq
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")

# ---------------------------------------------------------------- tin interior
TIN_X0, TIN_X1 = -45.37, 12.28
TIN_Y0, TIN_Y1 = 66.21, 159.01
TIN_R = 13.45
CLEAR = 0.30                     # plate-to-wall clearance per side (magnets hold)

# ---------------------------------------------------------------- vertical stack
CARRIER_TOP = 10.91              # carrier PCB top (Assembled.step, 21 Oct 23:45)
BATTERY_TOP = 8.75
GLASS_TOP = 16.00                # 24 Oct: 16.0 so the SH header tops (13.9) clear the glass back (14.2)
LIP = 1.00                       # bezel thickness over the glass border
PLATE_T = 1.60                   # plate thickness elsewhere
TOP = GLASS_TOP + LIP            # planar top face (16.5): prints face-down
UNDER = TOP - PLATE_T            # plate underside away from the glass (14.9)
GLASS_STACK = 1.80               # glass + front light; pocket depth below the lip
RIM_T = 1.20                     # pocket rim wall thickness

# ---------------------------------------------------------------- display glass
GLASS_W, GLASS_L = 36.7, 79.0
GLASS_X0 = -34.81                # 1 mm toward the hinge vs the Fusion placeholder
GLASS_Y0 = 70.81
AA_W, AA_L = 29.05, 66.89        # GDEY029T94 active area
AA_FAR_MARGIN = 3.5              # from the non-FPC (-y) glass edge; verify on part
WIN_CLEAR = 0.30
FPC_W = 24.0                     # opening in the +y rim wall for the FPC fold

# ---------------------------------------------------------------- carrier features (board -> world)
def kw(kx, ky):
    return (ky - 101.27, kx - 2.03)

# board edges in world: x -43.90..5.06, y 72.24..111.14 (24 Oct 2026 outline, 1.0 mm thick, no holes)
BOARD_X1 = kw(0, 106.33)[0]      # far-side edge (5.06)
BOARD_Y1 = kw(113.17, 0)[1]      # battery-end edge (111.14)
BOARD_T = 1.0
J6 = kw(87.55, 61.99)            # USB-C centre (-39.28, 85.52); its opening also locates the plate in x/y
J6_OPEN = (3.9, 10.9)            # (x, y) opening for the 3.2 x 10.3 receptacle
SW1 = kw(111.00, 62.10)          # tactile at the hinge/battery-end corner (-39.17, 108.97)
SW1_HOLE = 5.2                   # clears the FL_CAP_D cap, not just the stem
# FL button cap (TS-1109S-C-H: body 2.75 tall, stem top at CARRIER_TOP + 7.0, stem dia 2.30)
FL_STEM_TOP = CARRIER_TOP + 7.0
FL_STEM_D = 2.3
FL_CAP_D = 4.8                   # rides in SW1_HOLE
FL_CAP_TOP = 18.6                # CardKB key bottoms at 19.26 with the lid closed
FL_FLANGE_D = 6.0                # under the plate band: keeps the cap captive
FL_FLANGE_Z = (UNDER - 0.8, UNDER - 0.3)   # under the band; switch body top is 13.66
SW2 = kw(103.43, 62.20)          # JS102011SAQN footprint origin (-39.07, 101.40), travel along world y
# knob measured on the C&K model (24 Oct): 1.5 x 1.5 plan, 1.45 tall, top z 14.74, sitting 2.22 toward the
# hinge wall from the footprint origin; model shows it at one end of its +-1 mm travel
SW2_KNOB = (SW2[0] - 2.22, SW2[1])          # knob centre at mid travel (-41.29, 101.40)
SW2_KNOB_WH = (1.5, 1.5, 1.45)
SW2_SLOT = (4.0, 6.0)            # (x, y) centred on SW2_KNOB

# height datums resting on bare solder mask (no mounting holes on the board any more)
FOOT = kw(77.6, 59.6)            # hinge / Walter-end corner (-41.67, 75.57): inside the board's R5.45 corner arc, clear of C6
FOOT_D = 3.0
FOOT2 = None                     # e.g. (-41.8, 113.2): a third foot at the SW1 corner once the board extends to kx >= 116.5
RAIL_X = (3.4, 4.9)              # far-side edge strip outside the glass rim, inside the board edge
RAIL_Y = [(73.5, 79.0), (90.2, 103.5)]   # two segments: the board's SIM notch is at y 79.4..89.7; stops short of J2 (104.07)
BAT_FOOT = (-39.5, 114.0)        # 3 x 3 foot on the battery at the hinge / battery-end corner (wires pass at y >= 116)
BAT_FOOT_W = 3.0
# rotation stop: L-rib hanging past the far / battery-end board corner (USB-C fixes translation)
RIB_CLEAR = 0.2
RIB_T = 1.3
RIB_BOTTOM = 9.5                 # board bottom 9.91, battery top 8.75
RIB_FAR_Y = (100.0, None)        # along the far edge up to the corner
RIB_END_X = (4.2, None)          # along the battery-end edge, clear of J2's plug (x < 3)

LEG_D = 4.4                      # standoff on the 4.5 mm copper-free annulus
SCREW_HOLE = 2.4                 # M2 clearance
CBORE_D, CBORE_DEPTH = 4.0, 1.0  # M2 pan head

# ---------------------------------------------------------------- misc features
MAG_D, MAG_T = 6.3, 1.2          # pocket for a 6 x 1 mm disc magnet
TAB_W, TAB_T, TAB_BOTTOM = 9.0, 2.0, 8.0
MAG_Z = 11.5
WIRE_CH = (116.0, 124.0, 4.0)    # keyboard wire channel: y0, y1, depth into x
HINGE_NOTCH = [(88.0, 97.2), (128.0, 137.2)]   # lid hinge tabs, z 11.1..15.8
HINGE_NOTCH_DEPTH = 1.5
CRUSH_H, CRUSH_W = 0.45, 1.0     # crush ribs on the tab faces: 0.15 mm interference per side (sand to fit)

# ---------------------------------------------------------------- panel board holder (40 x 20 driver board)
# The 40x20 panel board slides in from +y into grooves in two posts hanging from the plate outside the
# glass rim, over the battery. Board rotated 180 deg in plane: its FPC edge (J1/J3/J4) faces +y, SH-8 (J2)
# faces -y toward the carrier's J1, SH-2 (J5) sits at the far end -> far post opened there for the plug.
PANEL_W, PANEL_L = 40.1, 20.1
PANEL_X0 = -36.5                 # centred on the glass (-16.45)
PANEL_Y0 = 128.0                 # -y edge; FPC fold between 148 and the +y rim wall at 150
PANEL_PCB_Z = 9.6                # PCB bottom: parts up to 12.57+1 = 13.57 < glass back 14.2; battery 8.75
PANEL_T = 1.0
POST_T, POST_LIP = 2.6, 0.8      # post thickness / how far the groove reaches over the board end
GROOVE_CLEAR = 0.3               # x clearance at the board end
POST_BOTTOM = PANEL_PCB_Z - 0.5  # groove floor 0.5 thick
DETENT_H = 0.3


def box(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False)
            .translate((x0, y0, z0)))


def cyl_z(cx, cy, d, z0, z1):
    return (cq.Workplane("XY").circle(d / 2).extrude(z1 - z0)
            .translate((cx, cy, z0)))


def rrect(x0, x1, y0, y1, r, z0, z1):
    w, l = x1 - x0, y1 - y0
    return (cq.Workplane("XY").center((x0 + x1) / 2, (y0 + y1) / 2)
            .rect(w, l).extrude(z1 - z0).edges("|Z").fillet(r)
            .translate((0, 0, z0)))


def build_plate():
    # 1. plate body
    px0, px1 = TIN_X0 + CLEAR, TIN_X1 - CLEAR
    py0, py1 = TIN_Y0 + CLEAR, TIN_Y1 - CLEAR
    plate = rrect(px0, px1, py0, py1, TIN_R - CLEAR, UNDER, TOP)

    # 2. glass pocket + rim
    gx0, gx1 = GLASS_X0 - 0.2, GLASS_X0 + GLASS_W + 0.2
    gy0, gy1 = GLASS_Y0 - 0.2, GLASS_Y0 + GLASS_L + 0.2
    rim_bottom = GLASS_TOP - GLASS_STACK
    rim = box(gx0 - RIM_T, gx1 + RIM_T, gy0 - RIM_T, gy1 + RIM_T, rim_bottom, UNDER)
    plate = plate.union(rim)
    plate = plate.cut(box(gx0, gx1, gy0, gy1, rim_bottom - 0.1, GLASS_TOP))
    # hinge-side rim removed where the SW1 cap flange (dia 6 at z 14.1-14.6) sweeps
    plate = plate.cut(box(gx0 - RIM_T - 0.1, gx0 + 0.01, 103.0, 113.0, rim_bottom - 0.1, UNDER + 0.01))
    # far-side rim removed over J2 (SH header top 13.88 > rim bottom 13.7)
    plate = plate.cut(box(gx1 - 0.01, gx1 + RIM_T + 0.1, 104.0, 111.0, rim_bottom - 0.1, UNDER + 0.01))
    # +y rim wall opened for the FPC fold
    gcx = (gx0 + gx1) / 2
    plate = plate.cut(box(gcx - FPC_W / 2, gcx + FPC_W / 2, gy1 - 0.01, gy1 + RIM_T + 0.1,
                          rim_bottom - 0.1, UNDER + 0.01))

    # 3. window over the active area
    wx0 = GLASS_X0 + (GLASS_W - AA_W) / 2 - WIN_CLEAR
    wx1 = wx0 + AA_W + 2 * WIN_CLEAR
    wy0 = GLASS_Y0 + AA_FAR_MARGIN - WIN_CLEAR
    wy1 = wy0 + AA_L + 2 * WIN_CLEAR
    plate = plate.cut(box(wx0, wx1, wy0, wy1, GLASS_TOP - 1, TOP + 1))

    # 4. hinge-band openings
    plate = plate.cut(box(J6[0] - J6_OPEN[0] / 2, J6[0] + J6_OPEN[0] / 2,
                          J6[1] - J6_OPEN[1] / 2, J6[1] + J6_OPEN[1] / 2, UNDER - 1, TOP + 1))
    plate = plate.cut(cyl_z(SW1[0], SW1[1], SW1_HOLE, UNDER - 1, TOP + 1))
    plate = plate.cut(box(SW2_KNOB[0] - SW2_SLOT[0] / 2, SW2_KNOB[0] + SW2_SLOT[0] / 2,
                          SW2_KNOB[1] - SW2_SLOT[1] / 2, SW2_KNOB[1] + SW2_SLOT[1] / 2, UNDER - 1, TOP + 1))

    # 5. height datums + rotation stop (board has no mounting holes)
    plate = plate.union(cyl_z(FOOT[0], FOOT[1], FOOT_D, CARRIER_TOP, UNDER + 0.01))
    if FOOT2:
        plate = plate.union(cyl_z(FOOT2[0], FOOT2[1], FOOT_D, CARRIER_TOP, UNDER + 0.01))
    for (ry0, ry1) in RAIL_Y:
        plate = plate.union(box(RAIL_X[0], RAIL_X[1], ry0, ry1, CARRIER_TOP, UNDER + 0.01))
    plate = plate.union(box(BAT_FOOT[0] - BAT_FOOT_W / 2, BAT_FOOT[0] + BAT_FOOT_W / 2,
                            BAT_FOOT[1] - BAT_FOOT_W / 2, BAT_FOOT[1] + BAT_FOOT_W / 2, POST_BOTTOM, UNDER + 0.01))
    rx0 = BOARD_X1 + RIB_CLEAR
    ry1 = BOARD_Y1 + RIB_CLEAR
    plate = plate.union(box(rx0, rx0 + RIB_T, RIB_FAR_Y[0], ry1 + RIB_T, RIB_BOTTOM, UNDER + 0.01))
    plate = plate.union(box(RIB_END_X[0], rx0 + RIB_T, ry1, ry1 + RIB_T, RIB_BOTTOM, UNDER + 0.01))

    # 6. panel board holder: two grooved posts over the battery, open toward +y
    x0, x1 = PANEL_X0, PANEL_X0 + PANEL_W
    y0, y1 = PANEL_Y0, PANEL_Y0 + PANEL_L
    gz0, gz1 = PANEL_PCB_Z, PANEL_PCB_Z + PANEL_T + 0.2
    for (px, sign) in ((x0, -1), (x1, +1)):
        inner = px + sign * (-POST_LIP)            # groove reaches this far over the board
        outer = px + sign * (POST_T - POST_LIP)
        plate = plate.union(box(min(inner, outer), max(inner, outer), y0 - 1.5, y1 + 0.5,
                                POST_BOTTOM, UNDER + 0.01))
        gx_in, gx_out = inner - sign * 0.01, px + sign * GROOVE_CLEAR
        plate = plate.cut(box(min(gx_in, gx_out), max(gx_in, gx_out), y0 - 0.3, y1 + 1.0, gz0, gz1))
        plate = plate.union(box(min(gx_in, gx_out), max(gx_in, gx_out), y1 + 0.3, y1 + 1.0,
                                gz0 - 0.01, gz0 + DETENT_H))
    # far post opened above the groove floor where the panel board's SH-2 (J5) plug exits (+x)
    plate = plate.cut(box(x1 - POST_LIP - 0.01, x1 + POST_T, y0 + 13.0, y1 + 1.1, gz0 + 0.01, UNDER - 0.01))

    # 7. magnet tabs: far long wall (+x) and both end walls
    for ty in (90.0, 135.0):
        tab = box(px1 - TAB_T, px1, ty - TAB_W / 2, ty + TAB_W / 2, TAB_BOTTOM, UNDER + 0.01)
        plate = plate.union(tab)
        for ry in (ty - 2.5, ty + 2.5):
            plate = plate.union(box(px1 - 0.01, px1 + CRUSH_H, ry - CRUSH_W / 2, ry + CRUSH_W / 2,
                                    TAB_BOTTOM, UNDER + 0.01))
        pocket = (cq.Workplane("YZ").center(ty, MAG_Z).circle(MAG_D / 2).extrude(MAG_T + 0.5)
                  .translate((px1 - MAG_T, 0, 0)))
        plate = plate.cut(pocket)
    for (ty0, ty1, dirn) in ((py0, py0 + TAB_T, -1), (py1 - TAB_T, py1, +1)):
        tx = -17.5
        plate = plate.union(box(tx - TAB_W / 2, tx + TAB_W / 2, ty0, ty1, TAB_BOTTOM, UNDER + 0.01))
        for rx in (tx - 2.5, tx + 2.5):
            ry0, ry1 = (ty0 - CRUSH_H, ty0 + 0.01) if dirn < 0 else (ty1 - 0.01, ty1 + CRUSH_H)
            plate = plate.union(box(rx - CRUSH_W / 2, rx + CRUSH_W / 2, ry0, ry1, TAB_BOTTOM, UNDER + 0.01))
        pocket = cq.Workplane("XZ").center(tx, MAG_Z).circle(MAG_D / 2).extrude(MAG_T + 0.5)
        # XZ workplane extrudes toward -y; place so the pocket opens on the outer face
        if dirn < 0:
            pocket = pocket.translate((0, ty0 + MAG_T, 0))
        else:
            pocket = pocket.translate((0, ty1 + 0.5, 0))
        plate = plate.cut(pocket)

    # 8. hinge-side notches for the lid's hinge tabs
    for (ny0, ny1) in HINGE_NOTCH:
        plate = plate.cut(box(px0 - 0.5, px0 + HINGE_NOTCH_DEPTH, ny0, ny1, UNDER - 1, TOP + 1))

    # 9. keyboard wire channel (rounded inner corners)
    wy0, wy1, wd = WIRE_CH
    ch = (cq.Workplane("XY").center(px0 - 2 + (wd + 2) / 2, (wy0 + wy1) / 2)
          .rect(wd + 2, wy1 - wy0).extrude(PLATE_T + 2).edges("|Z").fillet(1.5)
          .translate((0, 0, UNDER - 1)))
    plate = plate.cut(ch)
    return plate


def build_cap():
    # Slider cap for SW2 (JS102011SAQN). Head 7 (y) x 4 (x) x 1.2 rides on the plate (7 long so it clears
    # the FL cap at full +y travel); stem 3.6 (x) x 4.0 (y) slides +-1 mm in the 4.0 x 6.0 slot; pocket
    # grips the 1.5 x 1.5 knob with 0.1 per side (PLA shrink makes it a light press fit).
    stem_len = TOP - 13.6                       # stem bottom 0.3 above the switch frame top (13.3)
    kw_, kl_, kh_ = SW2_KNOB_WH
    head = box(-2.0, 2.0, -3.5, 3.5, 0, 1.2)
    stem = box(-1.8, 1.8, -2.0, 2.0, -stem_len, 0)
    cap = head.union(stem)
    cap = cap.cut(box(-kw_ / 2 - 0.1, kw_ / 2 + 0.1, -kl_ / 2 - 0.1, kl_ / 2 + 0.1,
                      -stem_len - 0.1, -stem_len + kh_ + 0.15))
    return cap.translate((SW2_KNOB[0], SW2_KNOB[1], TOP))   # world frame, like the other STEPs


def build_fl_cap():
    # Button cap for SW1 (TS-1109S-C-H, 7 mm). Press it onto the stem before the plate goes
    # on; the flange stays under the plate band so the cap cannot fall out. Modelled in the
    # world frame at the switch; prints flange-down as modelled.
    x, y = SW1
    cap = cyl_z(x, y, FL_CAP_D, FL_FLANGE_Z[0], FL_CAP_TOP)
    cap = cap.union(cyl_z(x, y, FL_FLANGE_D, FL_FLANGE_Z[0], FL_FLANGE_Z[1]))
    cap = cap.cut(cyl_z(x, y, FL_STEM_D + 0.1, FL_FLANGE_Z[0] - 1, FL_STEM_TOP + 0.1))
    cap = cap.edges(">Z").chamfer(0.4)
    return cap


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    plate = build_plate()
    cap = build_cap()
    cq.exporters.export(plate, os.path.join(OUT, "faceplate.step"))
    cq.exporters.export(cap, os.path.join(OUT, "slider_cap.step"))
    fl = build_fl_cap()
    cq.exporters.export(fl, os.path.join(OUT, "fl_cap.step"))
    bbf = fl.val().BoundingBox()
    flp = fl.translate((-bbf.xmin, -bbf.ymin, -bbf.zmin))
    cq.exporters.export(flp, os.path.join(OUT, "fl_cap_print.stl"), tolerance=0.02, angularTolerance=0.1)
    print("fl cap bbox z %.2f..%.2f  print %.2f x %.2f x %.2f" % (bbf.zmin, bbf.zmax, bbf.xlen, bbf.ylen, bbf.zlen))
    # print orientation: top face on the bed
    bb = plate.val().BoundingBox()
    pp = plate.rotate((0, 0, 0), (1, 0, 0), 180)
    bb2 = pp.val().BoundingBox()
    pp = pp.translate((-bb2.xmin, -bb2.ymin, -bb2.zmin))
    cq.exporters.export(pp, os.path.join(OUT, "faceplate_print.stl"), tolerance=0.02, angularTolerance=0.1)
    cp = cap.rotate((0, 0, 0), (1, 0, 0), 180)   # head down on the bed
    bbc = cp.val().BoundingBox()
    cp = cp.translate((-bbc.xmin, -bbc.ymin, -bbc.zmin))
    cq.exporters.export(cp, os.path.join(OUT, "slider_cap_print.stl"), tolerance=0.02, angularTolerance=0.1)
    v = plate.val()
    print("plate bbox x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f  volume %.0f mm3  solids %d" %
          (bb.xmin, bb.xmax, bb.ymin, bb.ymax, bb.zmin, bb.zmax, v.Volume(), len(plate.solids().vals())))
    print("print bbox", pp.val().BoundingBox().xlen, pp.val().BoundingBox().ylen, pp.val().BoundingBox().zlen)

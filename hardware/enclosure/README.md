# Enclosure: display cassette faceplate (second fit-check revision, 24 Oct 2026 pm)

One printed plate that closes the Altoids tin under the lid: bezel over the bare 2.9" glass, control band
on the hinge side (USB-C, front-light button, power slide), skirt tabs that press into the tin, and the
holder for the 40 x 20 panel board. Built from `pager-carrier.kicad_pcb` as of 24 Oct 15:32 (no mounting
holes, 38.9 x 49 mm outline, R5.45 corner at the hinge / Walter end, SIM notch in the far edge) and `panel/pager-panel.kicad_pcb`.

Source: `faceplate.py` (CadQuery 2.8; every dimension is a named constant at the top).
Run: `<cadquery python> faceplate.py` -> `out/`.

| file | what |
|---|---|
| out/faceplate.step | plate in the Fusion assembly frame (same frame as hardware/carrier/Assembled*.step) |
| out/faceplate_print.stl | plate flipped top-face-down, min corner at the origin, ready to slice |
| out/slider_cap.step / _print.stl | cap that rides in the SW2 slot and grips the 1.5 x 1.5 slide knob |
| out/fl_cap.step / _print.stl | captive button cap pressed onto the SW1 tactile stem (prints flange down) |
| out/panel_board_placed.step | KiCad export of the panel board, pre-placed where the holder puts it |
| out/carrier_placed.step | KiCad export of the current carrier, pre-placed at CARRIER_TOP 10.91 |
| fusion/UpdatePagerFaceplate/ | Fusion script: saves a version, swaps the printed parts + panel board, saves again |

## Printing (Bambu Lab A1, PLA, 0.4 mm nozzle)
- Slice the `_print.stl` files as exported: plate top face on the bed, no supports, 0.2 mm layers, 3 walls,
  4 top/bottom layers, 15 % infill is plenty (it is mostly walls). The panel-board grooves and the magnet
  pockets are small horizontal overhangs; they print without support.
- Crush ribs on the tabs stand 0.45 mm proud (0.15 mm interference per side against the tin). Sand them
  if the plate will not seat; add the magnets if it is loose.

## How the assembly goes together (no screws, no pegs)
1. Glass: double-sided tape into the pocket under the 1 mm lip (pocket depth GLASS_STACK 1.8).
2. Panel board: connect the FPCs to the glass, then slide the board in from the +y end into the grooves of
   the two posts (over the battery, under the glass). A 0.3 mm detent at the open end keeps it in.
3. Caps: press the FL cap onto the tactile stem (flange under the band) and drop the slide cap into its slot.
4. Lower the plate onto the soldered Walter+carrier unit. The USB-C shell in its opening fixes the plate in
   x/y; the L-rib outside the far / battery-end board corner fixes rotation; the foot at the hinge /
   Walter-end corner and the two rail segments along the far edge (either side of the SIM notch) rest on
   bare solder mask and set the height. A 3 x 3 foot at the hinge / battery-end corner and the two
   panel-board posts rest on the battery through a thin foam pad (0.35 mm nominal): support, not datum,
   because the pouch cell's height is not controlled.
   Plug the SH-8 cable from the carrier J1 to the panel board J2 before the plate is all the way down.
5. Press the plate into the tin: the four tabs with crush ribs grip the far and end walls (hinge-side
   clearance 0.3 mm). The lid and the magnets (optional) back that up.
The Walter+carrier unit and the battery are double-sided-taped to the tin floor as the owner planned;
the plate does not clamp them. A dab of hot glue between the far rail and the board makes plate+board one
unit if wanted.

## What the plate assumes (check each against hardware before trusting a fit)
- Carrier PCB top at z 10.91 (CARRIER_TOP, from the 21 Oct assembly, socketed headers). The soldered unit
  may sit lower (owner's 22 Oct assembly: 10.47; seated on the header bodies: ~9.75). Set CARRIER_TOP and
  rerun; feet, rail, rib and cap depths follow it.
- Glass top at z 16.0 (GLASS_TOP; plate top 17.0, plate underside 15.4). Raised from 15.5 on 24 Oct so the
  SH header tops (13.88) clear the glass back (14.2) and the panel board fits under the glass. Keys on the
  closed lid are at 19.26: FL cap top 18.6, slide cap top 18.2, USB-C shell 17.5.
- Bare GDEY029T94(-FL03) glass 79 x 36.7 at x -34.81..1.89, y 70.81..149.81, FPC on the +y end; active
  area 29.05 x 66.89 with a 3.5 mm far margin (AA_FAR_MARGIN; verify on the part).
- Carrier features from the 24 Oct board file: outline x -43.90..5.06, y 72.24..111.14 (world), USB-C J6
  (-39.28, 85.52), SW1 (-39.17, 108.97), SW2 origin (-39.07, 101.40) with its knob 2.22 toward the hinge
  wall, J2 keyboard header at (-0.05, 107.44) facing +y, J1 panel header (-16.3, 107.44) facing +y.
- SW1 is drawn for the XKB TS-1109S-C-H 7 mm tactile (LCSC C318908, stem 2.3 dia, body 2.75). The board
  still carries the KMR2 footprint; the TS-1109S 4.5 mm body overhangs the board edge by 0.08 mm at the
  current SW1 position, so shift SW1 0.2 mm toward SW2 when swapping the footprint.
- Panel board 40 x 20 x 1.0, rotated 180 deg in plane, PCB bottom at z 9.6 (0.85 above the battery, parts
  up to 13.57 under the glass back 14.2). FPC connectors face +y, SH-8 faces -y toward the carrier, SH-2
  (J5) at the far end where the far post is opened for its plug. FH34SRJ height assumed 1.0 mm (no model).
- Walter header pins trimmed after soldering (fillets ~1 mm; untrimmed pins reach z 15.6 and hit the rim).
- Tin interior from Assembled V0.2.step (57.65 x 92.8, R13.45, walls straight). Lid hinge tabs assumed to
  intrude at most 1.5 mm at y 88-97 and 128-137.

## Carrier outline vs the tin (24 Oct pm check)
Every Edge.Cuts point tested against the modelled tin interior: the new R5.45 corner arc at the hinge /
Walter end clears the tin by 1.25 mm (the earlier R1 corner poked out 0.23 mm); all other edges clear by
>= 1.47 mm (hinge side) and >= 6 mm elsewhere. kicad-cli DRC: 0 errors, 1 warning (library footprint
mismatch), 0 unconnected. The SIM notch (board x 81.41..91.75, y 89.92..106.30) spans the Walter's SIM
holder in y (91.3..103.5) and opens to the far edge; the holder's low-x end (80.8..81.4) stays 0.6 mm
under the board. The notch is under the glass, so SIM access means lifting the plate.

## Extending the board toward the battery
Not needed for the plate. If the right edge grows to kx >= 116.5 (world y 114.5), the strip beyond SW1
takes a third foot at (-41.8, 113.2) (FOOT2 in faceplate.py) and the plate stops cantilevering over the
hinge / battery-end corner. The corner rib follows BOARD_Y1 automatically. Keep kx <= 125 so the edge
stays clear of the panel-board posts (y >= 126.5).

## Verification (24 Oct)
Boolean intersection of the exported STEPs against the current board export placed at CARRIER_TOP and
against the panel board at its holder position: no overlaps. Closest approaches: USB-C shell 0.36 mm,
feet/rail touching the PCB (0.00 by design), panel PCB in its groove 0.09, panel J5 0.55, slide cap
pocket to knob 0.10 at the modelled knob position. Tin clearance 0.3 mm less the 0.45 mm crush ribs.

## Known gaps / next
- CARRIER_TOP must be confirmed from the soldered unit (or the owner's Fusion assembly export).
- Hinge-side band rests on the board only at the Walter-end foot; the battery-end corner rests on the
  battery foot (compliant). A board extension (FOOT2) would make that a hard datum.
- Slide knob position at mid travel is inferred (model shows one end of a +-1 mm travel).
- CardKB clamp rails (ridges that meet the keyboard PCB margins when the lid closes) not included.
- Keyboard wires: J2 is now at the far / battery-end corner; the wires cross under the plate over the
  battery to the hinge-edge channel at y 116-124.

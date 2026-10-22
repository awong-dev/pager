# Enclosure: display cassette faceplate (first fit-check revision, 22 Oct 2026)

`faceplate.py` is a parametric CadQuery model of the plate that holds the e-paper glass, closes the
Altoids tin's open top under the lid-mounted CardKB, and screws down onto the carrier. `out/` holds the
generated files; rebuild with a CadQuery 2.8 python (`pip install cadquery`, python 3.12):

    python faceplate.py

| file | use |
|---|---|
| out/faceplate.step | plate in the Fusion assembly frame (same frame as hardware/carrier/Assembled*.step) |
| out/faceplate_print.stl | plate flipped top-face-down, min corner at the origin, ready to slice |
| out/slider_cap.step, out/slider_cap_print.stl | cap that rides in the SW2 slot and grips the slide knob |

## Printing (Bambu Lab A1, PLA, 0.4 mm nozzle)
- Orientation as exported: the cosmetic top face is on the bed, everything else grows upward. No supports.
- 0.2 mm layers, 3 walls, 4 top/bottom layers, 20 % infill or more. Textured PEI gives the top face a finish.
- Thinnest features: 1.2 mm pocket rim, 1.0 mm bezel lip, 0.8 mm walls in the cap's fork. All print at 0.4.
- Magnet pockets are horizontal 6.3 mm holes in 2 mm tabs; PLA bridges them without support.

## What the plate assumes (check each against hardware before trusting a fit)
- Display = bare GDEY029T94(-FL03) glass 79.0 x 36.7, placed 1 mm toward the hinge from the Fusion
  placeholder (x -34.81..1.89, y 70.81..149.81), FPC leaving the battery-end short edge, centred, 24 mm wide.
  Glass top at z 15.5 (GLASS_TOP). Window = active area 66.89 x 29.05 with 0.3 mm clearance, 3.5 mm from the
  non-FPC end (AA_FAR_MARGIN; verify on the part).
- Pocket depth 1.8 mm (GLASS_STACK). At GLASS_TOP 15.5 that puts the glass back at 13.7, which is 0.26 mm
  BELOW the carrier's JST SH header tops (13.96). The bare 1.05 mm glass clears; the front-lit stack may not.
  If the FL03 stack is thicker than 1.5 mm, set GLASS_TOP to 16.0 (everything above the carrier moves up 0.5;
  keys still clear by 2.3 mm). J7 (JST PH, top at z 16.4) collides with any glass position and must be swapped
  on the carrier (see HANDOFF-v0.2.md).
- Carrier: PCB top z 10.91, holes H1 (-40.37, 75.37), H2 (-41.32, 109.17), H3 (4.58, 109.17), USB-C J6 centre
  (-40.48, 83.87), SW1 (-40.57, 92.92), SW2 (-38.97, 101.41), all from pager-carrier.kicad_pcb on 22 Oct 2026.
  Assembled.step's carrier is an older export (J6 2.5 mm off, no SW2, no holes); the plate follows the board file.
- SW1 is drawn for a 4.5 x 4.5 mm, 7 mm tall tactile (XKB TS-1109S-C-H, LCSC C318908) whose stem passes a
  3.6 mm hole and stands 1.4 mm proud of the plate. The KMR2 currently on the board stays 3.6 mm below the plate.
- Walter header pins trimmed flush; untrimmed they reach z 15.6 and hit the pocket rim.
- Tin interior from Assembled V0.2.step (57.65 x 92.8, R13.45, walls straight). Plate is 0.3 mm smaller per
  side; magnets, not friction, hold it. Lid hinge tabs assumed to intrude at most 1.5 mm at y 88-97 and 128-137.

## Hardware
- 4 x disc magnets 6 x 1 mm (N52), CA-glued in the tab pockets, outer face flush. Tin walls are steel.
- 3 x M2 screws through the plate into the carrier holes. The carrier holes are plain 2.2 mm; the screw only
  locates unless a nut sits under the carrier (fit before the carrier is soldered to the Walter) or the hole gets
  a threaded insert. For the first fit the magnets and lid carry the load.
- 2 mm double-sided tape on the glass border under the bezel lip. 1 mm foam under the two battery legs.

## Known gaps / next
- No tray yet: the glass back is unsupported except by tape, and the 40 x 20 panel board has no holder.
  The tray snaps under the plate, carries foam behind the glass and the panel board over the battery end.
- CardKB clamp rails (ridges that meet the keyboard PCB margins when the lid closes) not included.
- Slider cap pocket size is a guess at the JS102011SAQN knob; measure and set in build_cap().
- Verified by boolean intersection against the current carrier export and the V0.2 tin: no overlaps; the only
  sub-0.5 mm gaps are the intended clearances (USB-C 0.37, SW2 knob 0.15 under the plate).

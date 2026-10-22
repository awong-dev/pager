# Carrier board v0.2c — handoff addendum (21 Oct 2026, afternoon)

Read this section first; the v0.2b notes below still describe the tools, but the outline, decisions and pipeline
location changed. Session scratchpad: `/private/tmp/claude-501/-Users-albert-src-pager/30499713-6299-4751-a029-fb918966b264/scratchpad/layout2/`
(scripts re-pointed there; `geom.py` = v0.2c outline, `plan2.py` = anchors, `clean.py` draws the outline + eject hole +
WiFi keep-out rule area, `handroute2.py` = 2-layer USB-C corner + BQ25185 escapes + BRG_3V3, `prep2.sh`/`search2.sh`/
`post2.sh`/`final_check2.sh` as before). Snapshots of the v0.2b versions: `geom_v02b.py`, `plan_v02b.py`, `clean_v02b.py`.

## Owner decisions (21 Oct pm, binding)
- Only hard constraint: fit the Altoids tin. X: board edge at the Walter's edge (74.3), not just hugging the header.
  Right edge may grow a little (now 115.0). The e-ink sits above the carrier: the board may extend under it.
- Y: into the WiFi keepout "only a little"; prefer covering the SIM holder with a pin-eject hole over covering the keepout.
  v0.2c: y 57.4..108.4 on the right strip (x > 99.2), 106.3 (= keepout line) over the Walter; the band y > 106.3 is a
  no-copper rule area. SIM notch removed; 2.0 mm eject hole at (86.25, 93.2) over the holder's inner end — **verify
  against the physical Walter** (card inserted from the antenna end, push-pull holder, 1.95 mm under it).
- The M8 antenna stub enters the far long wall right of the Walter (x > 99.2); exact x range not given, so the right
  strip's far edge stays at 108.4 (4.9 mm from the wall). Ask before growing it.
- Still: 2 layers only, six cable headers face +X on the right edge, keep v0.2 uncommitted until reviewed.

## Tin geometry (from "Assembled v0.1.step", replaces the eyeballed R19 arc)
Mapping STEP world -> KiCad: kx = world_y + 2.03, ky = world_x + 101.27. Tin inner walls y 56.13 / 113.32, end wall
x 68.47 (tin runs to x 160.8); all corners R 13.22 inner, the two near the Walter centred (81.69, 69.35) and (81.69, 100.1).
Board arc: centre (81.69, 69.35), R 12.22 (1 mm margin); the bottom-left corner (74.3, 108.4) is inside the lower arc.
Walter datasheet: 3.3 mm carrier-to-Walter gap, 1.95 mm under the SIM holder; keepout 24.8 x 6 mm from the antenna end.

## Placement (plan2.py)
Right edge top->bottom: J2 (111.6, 61.65), J1 (111.6, 72.85), J7 (109.8, 84.75), J3 (111.6, 92.65), J4 (111.6, 98.85),
J5 (111.6, 105.05), all rot 90 (JST SH cable opening is +y in footprint coords -> +x). Top strip J6/SW2/SW1 unchanged.
Q1 auto-reset cluster moved to the bottom next to Walter pin 1 (Q1 (94.0, 63.2) B). U5 at (84.0, 71.6), C13 right above
it (86.0, 69.0). Bridge channel y > 92 between the rows kept free of auto parts; bottom side reserved over the SIM holder.
KiCad flips footprints top/bottom (mirror y) — plan2.rot_xy now models that.

## USB-C corner, 2 layers (handroute2.py)
VBUS ties A4-B9 / B4-A9 straight on F; D+ row tie = 0.15 mm F diagonal between the rows (from the pad edges, 0.23 mm to
the neighbours); D- row tie B7 -> V4 (81.2, 58.5) -> B.Cu -> VD (79.4, 65.0) -> F through the middle of D1 into pad 1;
A6/A7 down on F into D1.3/D1.1; D1.4/D1.6 -> VA/VB -> B.Cu -> U1.3/U1.4; D1.5 -> VC (router continues); A9 -> VA9 -> U1.7;
BRG_3V3 hand-routed C9 -> U1.6/U1.5 (the router could not reach them). Pre-route DRC: 0 errors.

## State handed over 21 Oct 2026 ~22:00 PDT (owner finishes by hand)
`pager-carrier.kicad_pcb` = run 6 / Freerouting variant 9, **raw**: router output + hand routes + GND pours, no stitching,
no repair passes (snapshot: scratchpad layout2/pcb_run6_v9_raw.kicad_pcb). DRC: 0 errors; 11 parked TPs; open:
SYS_SW x7 (whole net: M1.28, Q4.3, C6/C7, U7.2, R13, R35, R37, C19), Q1A-E1 x2, VBUS, SYS, U1-VBUS, U7-MODE, Q4-G,
U7-VSET, Q1B-E2 (1 each), GND x3 (islands; stitching was skipped). **Root cause of SYS_SW:** the GND via + stub added
above U7 pin 1 (88.45, 75.65) walls off U7 pin 2 (SYS_SW) in the routing copy, so every variant dropped the net —
delete that via/stub (or move it left of pin 1) before re-routing, or route SYS_SW by hand into pin 2 from the left.
Runs 3-5 were wasted by (a) GND-link keep-outs blocking neighbouring pins (fixed: thin keep-outs, mkdsn_2.py) and
(b) editing search2.sh while it ran (corrupted run 5's post-processing). Run 2's board (4 opens) was overwritten.
Lesson recorded in memory: snapshot every run, never edit a running pipeline script.

## Routing runs
Run 1 (old C13/VD): 6 variants, 9 unrouted at the router, after re-insertion 0 DRC errors and 11 open signal connections
(BRG_3V3 x6 unreachable, VBUS x2, CHG_STAT2, VBUS_DET, Q1A-B1) — far better than v0.2b's 7-14 whole nets.
Run 2 (search_c2.log): in progress at hand-off time; see the end of that log / final_check2.sh.

## USB-C 3D model
GCT's real STEP (TraceParts usb4235-03-c.stp) is now `pager-carrier.3dshapes/USB_C_Receptacle_GCT_USB4235_Vertical.step`
with `(rotate (xyz -90 0 0))` in both the library footprint and the board's embedded copy; renders upright on its pads.

---

# Carrier board v0.2 — handoff (21 Oct 2026)

Read this first in a fresh session. It replaces everything learned in the 21 Oct session about the v0.2 layout.

## Goal

Re-lay-out the Walter kid-pager carrier (hardware/carrier, KiCad 10.0.6) as small as possible. v0.1 (63 × 46 mm,
2-layer, fully routed, DRC clean) is committed at 85414e1 / tag `v0.1-carrier`. v0.2 is NOT committed; do not
commit it until the owner reviews it. Never run gen/build.py main, gen/carrier_pcb.py or finish_pcb.py carrier
(they overwrite the hand-edited carrier). Only the panel board is still generated.

## Owner decisions (binding)

- **2 layers only.** Width may grow to the right. If routing still fails after ~6 pipeline iterations, abort and rediscuss.
- The hat may cover the **full Walter width** (x 74.3..99.2); no benefit in hugging the pin rows.
- **No header may face the top or bottom edge** (the board is nearly flush with both tin walls). All six cable
  headers (J1 panel, J2 keyboard, J3 haptic, J4 LED, J5 piezo, J7 battery) face +X on the right edge.
- **Top-left corner must follow the tin's rounded end-wall corner** with a manufacturing margin. From the owner's
  STEP the corner radius is ~15–20 mm; the chosen arc is concentric with centre (87.7, 74.1), radius 19 (1 mm margin).
- The right strip (x > 99.2) overhangs the Walter and sits flush on the battery: **no bottom-side parts there.**
  Bottom-side parts are fine inside the Walter shadow (≤ ~1.1 mm tall: 0402/0603/QFN/VSSOP/SOT).
- Switches SW1/SW2 and the vertical USB-C J6 stay in the top strip (hinge edge), beside the display's footprint.
- Test points TP1–TP11 stay parked off-board (DRC shows them unconnected; that is accepted).
- Open question for the owner: keep the vertical GCT USB4235 (custom footprint, now with a placeholder box STEP in
  pager-carrier.3dshapes; fetch GCT's real STEP when online) or switch to a horizontal library part (GCT USB4105,
  3.2 mm, plug through a wall slot).

## Mechanical facts

- Altoids bottom ~94 × 60 × 21 mm. The Walter (55 × 24.8) sits crosswise at one end, **header side up**, components
  facing down onto the 2500 mAh battery; the carrier is a hat on the Walter's pins, ~2.5–3.4 mm above its back
  (back = nano-SIM holder + test pads). E-ink in the middle on top, CardKB in the lid, M8 antenna stub 2–3 mm into
  the non-hinge long wall.
- Board frame (KiCad mm): Walter header rows at x 77.85 / 95.63, pads y 66.93..99.95 (1.7/1.02 THT); Walter body
  x 74.3..99.2, y 57.2..112.3; SIM holder under x 81.4..91.1, y 91.2..103.6 → SIM notch x 80.9..92.96, open to the
  bottom edge, starting at y 90.0 (owner's v0.1 notch started at 85.47; shortened to free the bridge over the pins
  for routing — mention it in the report).
- Tin walls in board frame: hinge-side long wall y ≈ 54.1, far long wall y ≈ 114.1, end wall x ≈ 67.7.
- v0.2b outline (geom.py): x 74.3..113.0, y 57.4..108.4, 1 mm convex corners, tin arc at the top-left.

## Where things are

- Pipeline: session scratchpad `/private/tmp/claude-501/-Users-albert-src-pager/<session>/scratchpad/layout2/`
  (if the scratchpad is gone, the scripts must be recreated; the 21 Oct session id was df4dc366-ba40-4c7e-83de-0bfb3a07db72).
  Key scripts: `geom.py` (outline), `plan.py` (anchors + numpy auto-placer → placement.json), `clean.py` +
  `apply_place.py` (v01.kicad_pcb → placed.kicad_pcb), `handroute2.py` (scripted escapes), `gndvias.py`,
  `mkdsn_2.py` (routing copy: GND/TP detached, pads grown +0.05/side so the router's 0.15 clearance gives 0.2
  track-to-pad, GND vias → keep-outs, inset outline), `prep2.sh` → `search2.sh` (Freerouting net-order variants,
  `SEEDS="0 1 2 3 4 5" MP=60`) → `post2.sh` (SES import, re-insert pre-routed items, padreach, pours, fixnet2 ×3,
  stitch2 ×4, sliverfix, gndclean, final DRC) → `final_check2.sh`. Freerouting jar: scratchpad/freerouting/freerouting-2.5.0.jar.
- The project board file currently holds the 4-layer attempt (0 DRC errors, 5 open nets). The 2-layer v0.1 is in git.
- Memory notes: carrier-v02-dense.md, carrier-layout-pipeline.md, carrier-kicad-hand-edited.md.

## State of v0.2b (2-layer, new outline) — what is done / not done

Done: geom.py with the new outline + tin arc; clean.py draws it and sets 2 copper layers; mkdsn_2/fixnet2 back to
2-layer. NOT done: plan.py anchors for the new outline; handroute2.py 2-layer USB-C corner; any routing run.

Placement plan for plan.py (anchors are (x, y, rot, side)):
- Top strip: J6 (82.7, 60.79, 0) — the tin arc clears it; SW2 (93.3, 60.7), SW1 (100.85, 60.7); Q1 auto-reset
  cluster (Q1 r180, R39, R7, R8) in the now-free top-right (x 103..113, y 58..65).
- Right edge, facing +X, top to bottom: J2 7.9, J1 13.9, J7 9.3, J3 5.9, J4 5.9, J5 5.9 (48.8 mm; place SH at
  x = 113 − 3.3, the PH2 J7 at x = 113 − 5.15).
- Between the rows (top): D1 ESD under J6, C13/R23, U5 BQ25185 + R26/R27/R25/R24 stack (x 87.6) + C15, Q4,
  C6/C7, U7 + L1 + C8 + R36/R37, U4 + C12. Shift U5 ~2 mm lower than before (to y ≈ 71.6) to free the band under D1
  for the D1→U1 vias. Keep y > 84.3 between the rows free (the bridge over the pins is the channel for the leg nets).
- Bottom (inside the Walter shadow): U1 CP2102N (≈ 85.9, 65.8), U2 LVC (≈ 91.6, 67.0, r180) with ≥1 mm between
  their courtyards, R3, C9, C4, R14, R4, R6, R15/R16 next to the Walter pins 24/25, C11 under U3 pin 1.
- Keep the left strip x < 77 free of parts on both sides (routing channel to the leg pins).
- Right strip top side: U3 DRV2605, Q2/Q3/R12/R9/R10/R13 front-light driver near J1 pins 9/10, R11, U6 + C16/C17
  beside J7, R21 near J4, R19/R20/R22 near J5.

Hand routes that must exist before the router (handroute2.py):
- BQ25185 escapes (0.4 mm pitch): IN up to C13, STAT1/ISET/ILIM/TS with vertical-first jogs to the R stack
  (R26 r180 so its STAT1 pad faces U5), SYS to C15, VBAT and CHG_STAT2 to vias.
- USB-C corner, 2-layer topology: A4↔B9 and B4↔A9 VBUS ties (vertical, between the rows); A9 → via → U1 pin 7;
  A6 → via → D1.3 and A7 → via → D1.1 on top; B6/B7 → vias above the B row; the two row ties cross, so D− ties
  on B.Cu under the rows and D+ ties on B.Cu looping around the connector's LEFT end (x ≈ 76.5, keep y ≥ 59.6 there
  because of the tin arc); CC2 → R29 above the B row; CC1 → via → R31 on the bottom (top side has no room);
  VBUS to D1.5 from below between the D+/D− vias; D1.4/D1.6 → vias → U1 pins 3/4 (enter QFN pads along their axis).
  Iterate with prep2.sh + DRC (≈1 min per cycle) until the pre-route board has 0 errors, then route.

## Tool gotchas (all verified)

- pcbnew: one LoadBoard per process; don't iterate GetTracks after Remove(); `PAD.GetLayer()` ignores the
  footprint flip — use `pad.IsOnLayer(B_Cu)`; `GetParent()` is a container, use `GetParentFootprint()`.
- kicad-cli DRC must run inside hardware/carrier (project-local .kicad_pro/.kicad_dru); elsewhere it uses defaults.
- KiCad's SES import deletes all existing tracks/vias → re-insert pre-routed items (dump to JSON first).
- Freerouting 2.5: chaotic per net order (run 6 variants); `(type power)` is ignored; an exported `(plane …)` zone
  is honoured; a full-layer `(keepout` blocks vias; it routes to the enlarged pads so some wire ends stop ~0.05 mm
  short (padreach.py); fixed wires near enlarged pads count as "violations" but are harmless.
- The JLC rules (pager-carrier.kicad_dru): track-pad 0.2, track-track 0.127 (net class 0.15), track-PTH hole 0.33,
  edge 0.3, hole-hole 0.5. Entering a 0.5 mm-pitch QFN pad with a 0.2 track along the pad axis is fine (0.275).

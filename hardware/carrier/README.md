# pager-carrier and pager-panel

KiCad 10 projects for the carrier board that replaces the Adafruit breakouts
around the Walter module, and for the small panel board that drives the
2.9" SSD1680 e-ink glass. Both are generated from Python under `gen/`; the
generated `.kicad_*` files are committed and can be edited in KiCad from here
on (re-running the generator overwrites them).

Brief: the "Pager Carrier Board — KiCad Handoff" doc (17 Oct 2026). The owner
added two changes on 17 Oct: the fuel gauge is populated, and the e-ink ZIF and
boost cluster moved to a separate panel board that sits between the glass and
the battery, cabled to the carrier over an 8-way JST SH.

## Layout of this directory

| Path | What |
|---|---|
| `pager-carrier.kicad_pro/.kicad_sch/.kicad_pcb` | carrier project, root schematic, board |
| `power.kicad_sch` … `peripherals.kicad_sch` | the five hierarchical sheets from the brief |
| `pager-carrier.kicad_sym`, `pager-carrier.pretty/` | project symbols (Walter, BQ25185, TPS62840, LIS2DW12, DRV2605L, MAX17048, NSI45020, rail power symbols) and custom footprints |
| `panel/` | pager-panel project (ZIF + boost + 8-way SH), shares the symbol library above |
| `out/`, `panel/out/` | ERC and DRC reports, schematic PDF, BOM CSV, netlist, Freerouting DSN/SES |
| `gen/` | generator: `build.py` (schematics + exports), `carrier_pcb.py` / `panel_pcb.py` (boards, run with KiCad's python), `finish_pcb.py` |

Regenerate: `python3 gen/build.py` (schematics, ERC, PDF, BOM, netlist), then
`/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen/carrier_pcb.py --route`
(needs the Freerouting jar path in `gen/pcb.py`) and the same for `panel_pcb.py`.

## Custom footprints and where their numbers came from

- `Walter_Module_2x14_THT`: datasheet rev 1.1 (hw 1.6) section 6 plus the
  vendor `walter-solder.kicad_mod` in `~/src/walter-hardware`. Rows 17.78 mm
  apart, 2.54 pitch, pin 1 at 9.72 mm from the USB-C end, 1.02 drill / 2.0 pad.
  The carrier sits above a header-side-up Walter, so the pattern is the
  **mirror** of the vendor's: pin 1 is on the +X row as seen from the carrier
  top. SIM holder, u.FL jacks and USB-C are marked on User.Comments.
- `BQ25185_WSON-10_DLH0010A_2.2x2mm_P0.4mm_EP0.9x1.5mm`: TI drawing 4226298/A
  (datasheet SLUSF65B): pads 0.5 x 0.2 on 0.4 pitch, columns 2.1 mm apart,
  thermal pad 0.9 x 1.5 with two 0.2 mm vias. Matches the SnapEDA footprint in
  `~/Documents/KiCad/KidPager/BQ25185DLHR/` and KiCad's stock
  `Texas_DLH0010A_WSON-10-1EP_2.2x2mm_P0.4mm_EP0.9x1.5mm` pad for pad.
- `USB_C_Receptacle_GCT_USB4235_Vertical`: GCT drawing rev A (21 Aug 2024).
  16 pads 0.30 x 0.87 on 0.5 pitch at x = ±0.25/0.75/1.25/2.75, rows at
  y = ±0.87; plated shell slots 0.6 x 0.85 (1.1 x 1.35 copper) at x = ±5.0; NPTH
  pegs 0.6 mm at x = ±3.75. Height 6.5 mm. Only ordering code: USB4235-03-C.
- `TPS62840_VSON-HR-8_DLC0008B_1.5x2mm_P0.5mm`: TI DLC0008B land pattern, pads
  0.6 x 0.25 at x = ±0.65 (the stock SOT-583-8 is 0.09 mm wider per side).

## Mechanical placement (carrier, 85 x 52 mm, 1.0 mm FR4)

- Walter crosswise under the left 26 mm (its 55 mm axis runs across the 52 mm
  board width); header centre at (13.5, 24.73). USB-C end of the Walter at the
  top edge, antenna/SIM end at the bottom edge.
- Antenna notch 12 x 6 mm at the top-left edge (x 2.5–14.5): the u.FL jacks are
  at the Walter's **USB-C end**, not the antenna end as the brief assumed.
  3 mm all-layer keep-outs around both jacks.
- Top strip (y 0–8): vertical USB-C J1, slide switch SW1, panel cable J3,
  front-light J5, battery J2 (cables exit toward the tin wall).
- Glass region (y 8–44.7, x 0.5–79.5): parts ≤ 2 mm only. Right end
  (x 62–79.5) left empty for the panel board.
- Bottom strip (y 44.7–52): keyboard J4, LED J6 and haptic J7 (DNP), bench LED
  D5, the 14 test pads.
- Walter region, top side only, between the header rows: CP2102N cluster,
  SN74LVC2G126 + UMH3N auto-reset, BQ25185 cluster, P-FET switch parts.
- Mounting holes H1–H4 at (17, 2.5), (82.5, 2.5), (82.5, 49.5), (2.5, 49.5).

## Deviations from the brief and interpretations

1. **CP2102N package**: the GQFN20 has no DTR pin, so auto-reset is
   impossible with it. U5 is **CP2102N-A02-GQFN24** (DTR pin 23, RTS 19,
   VIO tied to VDD, RSTb pull-up 1 k per datasheet, VBUS sense via
   22.1 k / 47.5 k, 4.7 µF + 100 nF per supply pin).
2. **TPS62840DLCR is not a fixed 3.3 V part**: VSET takes a 267 k resistor to
   GND for 3.3 V; pin 8 is VOS (output sense), wired to 3V3.
3. **PWR_FLAG**: SYS is driven by the charger's SYS power-output pin, so a
   flag there is an ERC error and was dropped; the other rails keep theirs.
4. **TPS22917 QOD** left open (open-collector to power-output is an ERC
   error); tie to VOUT in layout if quick discharge of the panel rail is wanted.
5. **Panel reference circuit** follows the GDEY029T94 spec sheet (24.10.22),
   not the brief's placeholders: RESE 2.2 Ω, L1 47 µH ≥ 0.43 A (cjiang
   FNR3015S470MT, 3 x 3 x 1.5 mm), 1 MΩ GDR pull-down, FPC pin 5 = VSH2 and
   pin 20 = VSH1 (both 1 µF/25 V), VPP open. Diode orientation as the brief
   described (D2 anode on the pump node, D1 anode on VGL).
6. **ISET** is 604 Ω (600 Ω is not an E96 value): 497 mA. ILIM/VSET 18 k gives
   the 500 mA limit (475 mA typ) with 4.20 V; the DNP alternate is 13 k
   (1.1 A), not 1 A.
7. **TS/MR**: a single 10 k to GND, per the datasheet "TS not used" text.
8. **NSI45020AT1G** pin 1 is the cathode (banded end). It needs ~1.8 V of
   headroom, so a ~3 V white front light only reaches 20 mA on USB power.
9. **Front light** parts are populated (the BOM table marks them fitted even
   though the scope paragraph lists the front light among DNP sub-circuits).
10. **Fuel gauge** U10 is populated (owner, 17 Oct). CELL and VDD both on VBAT,
    QSTRT and CTG to GND, EP to GND.
11. **MAX17048 footprint**: stock `TDFN-8-1EP_2x2mm_P0.5mm_EP0.8x1.2mm`
    (matches the Maxim 21-0168 drawing); DRV2605L uses `TSSOP-10_3x3mm_P0.5mm`
    (TI DGS land within 0.05 mm).
12. **Walter mounting**: the datasheet has no header-side-up guidance; the
    nano-SIM then faces the carrier and loads from the antenna end, which is
    within ~1 mm of the tin wall. Fit the SIM before mounting.
13. **References**: Q3 of the brief is the second half of Q2 (one UMH3N); the
    carrier's 8-way display connector is J3 (the brief's J3b); the panel board
    owns the brief's J3 ZIF, L1, Q5, D1–D3, R4, C3–C5.
14. **BOM**: SN74LVC2G126DCUR (VSSOP-8) and USB4235 are not stocked by
    LCSC/JLCPCB (hand-place; DCTR is MSOP-8 0.65 mm, a different footprint).
    LCSC numbers in the BOM were verified on jlcpcb.com/lcsc.com on 16–17 Oct
    2026; basic parts preferred.
15. **Routing** is Freerouting's output with GND pours on both layers; the
    boost loop on the panel and the USB D+/D− pair deserve a manual pass.

## Open questions (still open after this pass)

- Tin interior height with the lid closed versus 8.5 + 1 + 6.5 (USB4235) mm.
- GDEY029T94 contact face after the 180° fold: the drawing marks the contact
  side on the display-front face of the tail, which gives bottom contact
  (FH12-24S-0.5SH) when folded under; confirm on a physical panel.
- Walter header geometry against a physical module before ordering.
- Battery lead polarity (Adafruit 328) versus J2 pin 1.
- Front-light accessory part number, Vf and current (U8 is a 20 mA placeholder).
- LTE peak current at VIN on the bench, to confirm 100 µF + 10 µF and the
  P-FET drop at a 3.4 V cell.
- Where exactly the panel board sits relative to the glass tail (the carrier
  keeps x 62–79.5 under the glass clear for it).

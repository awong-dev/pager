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
| `pager-carrier.kicad_sym`, `pager-carrier.pretty/` | project symbols (Walter, BQ25185, TPS62840, LIS2DW12, DRV2605L, MAX17048, rail power symbols) and custom footprints (Walter, BQ25185, TPS62840, USB4235, FH34SRJ 24S/6S) |
| `panel/` | pager-panel project (display ZIF, front-light ZIF, boost, 10-way SH), shares the symbol library above |
| `out/`, `panel/out/` | ERC and DRC reports, schematic PDF, BOM CSV, netlist, Freerouting DSN/SES |
| `gen/` | generator: `build.py` (schematics + exports), `carrier_pcb.py` / `panel_pcb.py` (boards, run with KiCad's python), `finish_pcb.py` |

Regenerate: `python3 gen/build.py` (schematics, ERC, PDF, BOM, netlist), then
`/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen/carrier_pcb.py --route`
(needs the Freerouting jar path in `gen/pcb.py`) and the same for `panel_pcb.py`.

## Custom footprints and where their numbers came from

- `Walter_Module_2x14_THT`: datasheet rev 1.1 (hw 1.6) section 6 plus the
  vendor `walter-solder.kicad_mod` in `~/src/walter-hardware`. Rows 17.78 mm
  apart, 2.54 pitch, pin 1 at 9.72 mm from the USB-C end, 1.02 drill / 1.7 pad.
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
- `Hirose_FH34SRJ-24S-0.5SH` and `-6S-`: Hirose FH34SRJ catalog (Nov 2018)
  page 4 land pattern: contact pads 0.3 x 0.8 on 0.5 pitch, fitting pads
  0.4 x 0.8 at the E/F spans 3.3 mm behind the contacts; body D wide, 3.8 deep,
  **1.0 mm high**, top and bottom contact, back-flip actuator. Pin 1 at -X with
  the contacts (FPC entry) at -Y, the same convention as KiCad's FH12 footprints.
  KiCad 10 ships no FH34SRJ footprints.

## Mechanical placement (carrier, 85 x 52 mm, 1.0 mm FR4)

- Walter crosswise under the left 26 mm (its 55 mm axis runs across the 52 mm
  board width); header centre at (13.5, 24.73). USB-C end of the Walter at the
  top edge, antenna/SIM end at the bottom edge.
- Antenna notch 12 x 6 mm at the top-left edge (x 2.5–14.5): the u.FL jacks are
  at the Walter's **USB-C end**, not the antenna end as the brief assumed.
  3 mm all-layer keep-outs around both jacks.
- Top strip (y 0–8): vertical USB-C J1, slide switch SW1, 10-way panel cable
  J3, battery J2 (cables exit toward the tin wall).
- Glass region (y 8–44.7, x 0.5–79.5): parts ≤ 2 mm only. Right end
  (x 62–79.5) left empty for the panel board.
- Bottom strip (y 44.7–52): keyboard J4 (cable up to the CardKB in the lid),
  LED J6 and haptic J7 (DNP), the 14 test pads (1.5 mm pitch), and the
  front-light button SW2 at the right end (80, 48.4), marked LIGHT.
- Walter region, top side only, between the header rows: CP2102N cluster,
  SN74LVC2G126 + UMH3N auto-reset, BQ25185 cluster, P-FET switch parts.
- Mounting holes H1–H4 at (17, 2.5), (82.8, 2.2), (82.8, 43.5), (2.5, 49.5).

## Part heights (owner: low profile is critical; CardKB lives in the lid)

| Part | Height above its board |
|---|---|
| J1 USB4235 vertical USB-C | 6.5 mm (owner's choice; the horizontal fallback is 3.2) |
| J2, J4 JST PH side-entry (S2B/S4B-PH-SM4-TB) | 5.5 mm (6.0 deep; JST ePH catalog p.4) |
| SW1 slide switch JS102011SAQN | 3.5 mm |
| J3 JST SH side-entry SM10B-SRSS-TB | 2.9 mm |
| SW2 tactile SKQGAFE010 | 1.5 mm (sits beside the glass, below the glass top) |
| everything under the glass | ≤ 1.3 mm: 0805 caps 1.25, SOT-23 1.1, QFN 0.9, L2 1.0 (the 1210 100 µF at 2.5 mm is gone) |
| glass GDEY029T94-FL03 | 2.01 mm incl. the front-light plate |
| panel board | 1.0 mm FR4 + 1.5 mm (L1) ; FH34SRJ ZIFs 1.0, SOD-123 1.1 — a 0.8 mm board is fine |

So above the carrier: glass stack ≈ 1.3 + 2.0 (or panel 1.0 + 1.5 + 2.0 at the
tail end), and the USB-C at 6.5 is the only thing taller than 3.5 mm.

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
8. **Front light = GDEY029T94-FL03** (owner, 17 Oct): the light has its own
   6-pin 0.5 mm flex (pins 1-2 anode, 3-4 NC, 5-6 cathode; 4 LEDs in parallel,
   Vf 2.75/2.8/3.3 V, 60 mA max; 3.5 mm wide, ~30 mm long, leaves the glass at
   the corner next to the display tail). Its ZIF (panel J3, FH34SRJ-6S) is on
   the panel board and the anode/cathode ride the panel cable (pins 9/10), so the
   carrier's J5 is gone and the cable is a 10-way SH. The brief's NSI45020 CCR
   is dropped (needs 1.8 V headroom, and was a 20 mA part): R-FL is a 33 Ω 1206
   from SYS_SW (18 mA at a 3.4 V cell, 42 mA at 4.2 V, 50 mA on USB); dim or
   compensate with FL_PWM. 27 Ω is the brightest safe value (60 mA on USB).
9. **Front-light button** SW2 (owner, 17 Oct): FL_BTN on IO4 (header pin 15,
   RTC GPIO so it can be an ext1 wake), 100 k pull-up to 3V3, 100 nF, switch to
   GND. IO4 was a spare test-pad pin; TP13 still touches it. Firmware needs a
   `pins.h` entry for it.
10. **Fuel gauge** U10 is populated (owner, 17 Oct). CELL and VDD both on VBAT,
    QSTRT and CTG to GND, EP to GND.
11. **MAX17048 footprint**: stock `TDFN-8-1EP_2x2mm_P0.5mm_EP0.8x1.2mm`
    (matches the Maxim 21-0168 drawing); DRV2605L uses `TSSOP-10_3x3mm_P0.5mm`
    (TI DGS land within 0.05 mm).
12. **Walter mounting**: the datasheet has no header-side-up guidance; the
    nano-SIM then faces the carrier and loads from the antenna end, which is
    within ~1 mm of the tin wall. Fit the SIM before mounting.
13. **References**: Q3 of the brief is the second half of Q2 (one UMH3N); the
    carrier's 10-way display connector is J3 (the brief's J3b); the panel board
    owns the brief's J3 ZIF (as J1), L1, Q5, D1–D3, R4, C3–C5, plus the
    front-light ZIF as J3.
14. **BOM**: SN74LVC2G126DCUR (VSSOP-8) and USB4235 are not stocked by
    LCSC/JLCPCB (hand-place; DCTR is MSOP-8 0.65 mm, a different footprint).
    LCSC numbers in the BOM were verified on jlcpcb.com/lcsc.com on 16–17 Oct
    2026; basic parts preferred.
15. **Routing** is Freerouting's output (GND routed as tracks, then pours on
    both layers). After the 17 Oct morning changes both boards route
    completely: carrier and panel DRC show 0 unconnected, 0 clearance and
    0 courtyard errors; what remains is silkscreen-over-pad/overlap notes and
    a starved-thermal warning on the USB-C shell and one panel pad. The boost
    loop on the panel and the USB D+/D− pair still deserve a manual look.

## Sleep-current audit (owner, 17 Oct: "very low powered during sleep")

Battery only, SW1 on, Walter asleep. Carrier parts, from the datasheets:

| Consumer | Sleep current | Note |
|---|---|---|
| BQ25185 battery quiescent (IQ_BAT) | 4 µA typ, 5 max | unavoidable while the cell is connected |
| MAX17048 | 3 µA hibernate (23 µA active) | firmware must leave HIBRT at its default so it auto-hibernates, or force hibernate |
| TPS62840 3V3 buck | ~0.1 µA (36 nA VIN + 56 nA VOS) | |
| TPS22917 (3V3_EPD off) | 10 nA | 0.5 µA when on |
| LIS2DW12 low-power mode 1 | ~1 µA | shake wake; 50 nA if powered down |
| Q1 gate network | **1.9 µA** (was 38 µA) | 100 k pull-up changed to 2.2 M; the ON switch pulls the gate low through 10 k + pull-up |
| SN74LVC2G126 U6 | **0** (was up to ~10 µA) | VCC moved from 3V3 to BRG_VDD (CP2102N regulator), so it is unpowered without USB; Ioff outputs, ESP_RX0 held by its 10 k |
| CP2102N | 0 | bus powered |
| STAT/ACC/RX0/FL_BTN pull-ups, VBUS dividers | 0 | all idle high or on VBUS |
| front light, Q4, keyboard rail, panel rail | 0 | switched off |

Carrier total ≈ 10 µA on top of the Walter's own deep-sleep draw. Nothing
else on the carrier is worth a part swap; the fuel gauge is the biggest
remaining item and it is the one the owner asked to keep.

## TPS62840 fixed-voltage question

No. Every TPS6284x orderable (TPS62840 1.8–3.3 V, TPS62841 0.8–1.55 V,
TPS62842 1.8–3.6 V; DLC, YBG and DGR packages) sets its output with the
VSET resistor; there is no fixed-3.3 V code. The 267 k 0603 stays.

## Open questions (still open after this pass)

- Tin interior height with the lid closed versus 8.5 + 1 + 6.5 (USB4235) mm.
- GDEY029T94 tail pin-1 side after the 180° fold. The FH34SRJ has contacts
  on both faces, so the fold cannot leave the tail unconnected, but a wrong
  face mirrors the pin order (1↔24). The footprint keeps the FH12
  bottom-contact convention from the first pass; confirm on a physical panel
  before ordering. Same for the 6-pin front-light tail (a mirror there only
  swaps anode and cathode: no light, no damage).
- Walter header geometry against a physical module before ordering.
- Battery lead polarity (Adafruit 328) versus J2 pin 1.
- LTE peak current at VIN on the bench, to confirm 100 µF + 10 µF and the
  P-FET drop at a 3.4 V cell.
- Where exactly the panel board sits relative to the glass tail (the carrier
  keeps x 62–79.5 under the glass clear for it).

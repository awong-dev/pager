# Carrier / panel cost-reduction notes (4 Nov 2026)

Exploration only. Nothing in the schematics has been changed. Figures are per
unit at a 5-unit build, JLCPCB Economic assembly with the $1.50 feeder fee per
extended line ($0.30 per board at qty 5), and include the Walter at $85 landed.

Today: **$239 per unit** (Walter $85, Digi-Key parts + shipping $72, display
$14, two JLC boards landed $68).

Decisions already taken in the discussion:

- Keep the panel board separate (e-ink glass distance). No merge.
- Keep the carrier USB-C + bridge. The Walter's own USB-C cannot be used
  (VBUS/VIN tied on the module, port position fixed).
- Keep the MAX17048 (cell-side reading, SOC while charging, no modem wake).
- Keep the BQ25185 for now (power path, clean termination, 500 mA port limit).
- Remove GNSS (patch + U.FL cable, $6.35).

## Board-level swaps (carrier)

| Ref | Today | Proposed | Saves / board | Notes |
|---|---|---|---|---|
| U7 | TPS62840DLCR (C2071859, $3.83) | **TPS62842DGRR** (C2873354, $0.40) | $3.43 | Same die/family, 60 nA Iq, 750 mA. HVSSOP-8 3x3x1.1 mm, pin order differs (new footprint). No MODE pin (ours is tied to GND anyway). 3.3 V needs VSET 52.3 k (window 50.21–54.39 k; 51 k basic is inside it). LCSC prices the DLC at ~4x TI list; TI 1ku price is ~€0.95 for both. |
| U1 | CP2102N-A02-GQFN24 (C969151, $2.03) | **CH343P** (C2846043, $0.98) or CH340E (C99652, $0.68) | $1.05–1.35 | Both have DTR+RTS; UMH3N auto-reset stays. Bus powered, no sleep impact. |
| C6, C7 | 2x 100 µF 0805 (C141660, $0.39 ea) | 2x 22 µF 0805 basic | $1.02 | Walter datasheet asks for 2x 10 µF. Keep 100 µF if LTE brownouts were seen on the bench. |
| R27, R38 | 604 Ω (C8369), 267 k (C270622), both non-preferred extended | 620 Ω and 51 k basic | $0.60 | ISET becomes 484 mA (was 497). 267 k is moot with the TPS62842. |
| SW1 | KMR221GLFS (C72443, $0.69) | Alps SKQGAFE010 (C202424, $0.14) | $0.55 | Original 1.5 mm spec. TS-1187A-S16 (C42420254, $0.02) if a cheaper 5 mm tact is acceptable. |
| Q2 | SI1308EDL (C469327, $0.26) | AO3400A basic (C20917, $0.09) | $0.47 | Front-light switch only. Keep SI1308EDL on the panel boost (Q5). |
| Q3 | MMST3904 SOT-323 (C54879) | MMBT3904 SOT-23 basic (C20526) | $0.31 | Removes a fee. |
| D1 | USBLC6-2P6 (C2827693, $0.16) | USBLC6-2SC6 clone (C2687116, $0.05) | $0.11 | Still extended. |

Tier 2 total: about **$7.50 per board**. None of these change battery-side
sleep current (bridge is bus powered, buck Iq identical, VSET read once at
enable, MLCC leakage nA).

Still extended, left alone: MAX17048, BQ25185, DRV2605L (LRA mandatory),
LIS2DW12TR-MS, JST SH headers, USB-C, MSS12C02LS, 2G126, UMH3N, 2.2 µH.

Optional, not taken: TP4056 (C16581, preferred, no fee) in place of BQ25185
saves $1.70 but loses the power path (load on the cell during charge, 4.2 V
float, no 500 mA port limit, no boot from a flat cell).

## Off-board changes

| Item | Today | Proposed | Saves |
|---|---|---|---|
| LTE antenna | ANT-LTE-MON-SMA-L $13.54 + U.FL–SMA pigtail $5.03 | Molex 2091420180 FPC U.FL, 698–4000 MHz, $3.39 (Taoglas FXUB63 ~$5–6) | $15.18 (loses 617–698 MHz / band 71 only) |
| GNSS | GP.1575.25.2.A.02 $4.65 + U.FL–U.FL $1.70 | removed | $6.35 |
| Cables | JST A02SR x2, A08SR, Qwiic = $5.35 | generic SH 1.0 mm assemblies ~$0.30–0.50 ea | ~$3.85 |
| Piezo | CEB-20D64 $1.82 | dropped (no driver, LRA covers it) | $1.82 |
| Battery (optional) | Adafruit 2500 mAh $14.95 | protected 2000 mAh 103450 ~$10 | ~$5, 20 % less runtime |

## Off-board parts from LCSC, combined with the JLCPCB order (6 Nov 2026)

LCSC ships combined with a JLCPCB order: attach the JLCPCB order number at
LCSC checkout; same account and currency; the JLCPCB order must be addressed
to LCSC's Hong Kong warehouse at JLCPCB checkout (not changeable later);
shipping is recalculated as one parcel; the pair cannot be split afterwards.
LCSC's custom cable service (cable@lcsc.com, quote based) combines the same
way. The current PCBA run has already shipped, so this is for the next build.

Stock equivalents found (JLCPCB/LCSC catalog, 6 Nov 2026):

| Item | Digi-Key today | LCSC | Price | Notes |
|---|---|---|---|---|
| Piezo CEB-20D64 | $1.82 | FUET FT-20T-6.5A1, C56374389 | $0.13 | 20 mm, 6.5 kHz, 15 nF, 45 mm red/black leads. Direct equivalent. CEB-20D64 itself is C3812396, out of stock. |
| LRA VG0825001U | — | Leader LD0825BC-0168F, C5632413 (stock 41) or -0169F, C5632412 (160) | $1.63 / $1.22 | 8 mm, 2.55 mm thick, 1.2 V, leads. Resonant frequency not yet confirmed from the datasheet; DRV2605L auto-cal covers a near miss. |
| 2-way pigtails J7/J4/J5/J3 | half a K152B, ~$1.30 | BXCONN SH2P-28AWG-200-A1T, C53413423 | $0.07 | SH housing one end, bare other end, 200 mm. |
| CardKB 4-pin pigtail | Adafruit 4210, $0.95 | BXCONN SH4P-28AWG-200-A1T, C53413421 | $0.11 | Single-ended; pin order set at the CardKB solder end. |
| Ext LED | — | Everlight 204-10SURD, C84774 | $0.04 | 3 mm red. |
| 2x14 pin header | — | BOOMELE 2.54 2x14, C335179 | $0.17 | |
| LTE FPC antenna (if switched) | Molex $3.39 | Molex 2091420180, C6050372 | $3.20 | Only 2 in stock when checked. |
| U.FL to SMA pigtail | W9003M $5.03 | Kinghelm KH-IPEX-SMAKWE, C504006 | $1.17 | 100 mm, 3 GHz. Confirm bulkhead-nut style before relying on it. |

Not available or not suitable:

- Kinghelm FPC "700-2700" antennas ($0.39, C56127473 and siblings) are
  specified 800–2700 MHz in their own datasheet: miss US bands 12/13. No.
- 8-way and 2-way SH-to-SH jumpers (J1 to panel J2, J8 to panel J5): not
  stock items (only single-ended 2P–6P and a 3P double-ended lead). Keep the
  JST K51B from Digi-Key or get an LCSC custom-cable quote.
- Battery: no lithium pouch cells in the catalog.
- CardKB, Walter, GDEY029T94, SIM, M2 screws: not carried (only a Waveshare
  2.9 inch module, out of stock).

Net: piezo, four pigtails, CardKB lead, LED and header move to LCSC for about
$0.60 per pager against about $8 at Digi-Key, plus the LRA price difference.
The residual Digi-Key line (battery, two SH jumpers, CardKB, SIM) still needs
its own shipping unless batched.

Earlier cable row above is superseded: the main cable path is the Digi-Key
JST K51B jumpers (adds ~$3.50 to the totals below, ~$221 / ~$206), with the
LCSC single-ended pigtails as the cheaper option for the next build. The
piezo is kept, not dropped.

## Resulting per-unit cost (qty 5, Walter included)

| Scenario | Per unit |
|---|---|
| Today | $239.15 |
| GNSS removed + cables + piezo + Tier 2 swaps + ~$2 tariff follow-on, current SMA antenna kept | **$217.60** |
| Same, with the Molex FPC antenna | **$202.40** |
| Either, plus the $10 battery | −$5 |
| Either, plus TP4056 | −$1.70 |

Fixed costs (setup, stencil, fees, three vendors' shipping) are ~$140 per
order set; building 10 instead of 5 saves roughly $14 per unit on top.

Sources: Walter datasheet §5.1 (3V3 OUT is 250 mA, 1 µF max, switched by
IO0, so the carrier's own always-on 3.3 V rail stays), TI TPS62840 datasheet
rev with TPS62842DGR (device comparison table, VSET table 1), JLCPCB parts
API lookups 4 Nov 2026, Digi-Key listings for the Molex and Taoglas antennas.

"""pager-panel: the small display board between the e-ink glass and the
battery. Carries the GDEY029T94 FPC ZIF, the SSD1680 boost/charge-pump
cluster from the Good Display reference circuit, and the 8-pin JST SH cable
connector to the carrier."""

from carrier import Grid, Counter, res, cap, fields, two_pin, grp, C_FP, C0805, SH8

ZIF_FP = "Connector_FFC-FPC:Hirose_FH12-24S-0.5SH_1x24-1MP_P0.50mm_Horizontal"

# GDEY029T94 spec (24.10.22) FPC pin table
FPC_PINS = {
    1: "NC", 2: "EPD_GDR", 3: "EPD_RESE", 4: "NC", 5: "EPD_VSH2", 6: "NC", 7: "NC", 8: "GND",
    9: "EPD_BUSY", 10: "EPD_RST", 11: "EPD_DC", 12: "EPD_CS", 13: "EPD_SCK", 14: "EPD_MOSI",
    15: "3V3_EPD", 16: "3V3_EPD", 17: "GND", 18: "EPD_VDD", 19: "NC", 20: "EPD_VSH1", 21: "EPD_VGH",
    22: "EPD_VSL", 23: "EPD_VGL", 24: "EPD_VCOM",
}
FPC_NAMES = {1: "NC", 2: "GDR", 3: "RESE", 4: "NC", 5: "VSH2", 6: "TSCL", 7: "TSDA", 8: "BS1", 9: "BUSY",
             10: "RES#", 11: "D/C#", 12: "CS#", 13: "SCL", 14: "SDA", 15: "VDDIO", 16: "VCI", 17: "VSS",
             18: "VDD", 19: "VPP", 20: "VSH1", 21: "VGH", 22: "VSL", 23: "VGL", 24: "VCOM"}


def build(prj, lib):
    cnt = Counter()
    for r in ("R4", "C3", "C4", "C5"):
        cnt.use(r)
    sh = prj.add_sheet("panel", "panel_main.kicad_sch", paper="A3",
                       title="SSD1680 panel board: ZIF, boost and charge pump, cable to carrier")
    sh.text("Panel board for the Good Display GDEY029T94 (2.9\" 296x128, SSD1680). Reference circuit from the GDEY029T94 specification (24.10.22): RESE 2.2R, L 47uH, MBR0530 x3, 25 V caps.", (20, 20), 1.5, True)
    # --- J1 ZIF -----------------------------------------------------------
    j1 = sh.place("J1", "Connector_Generic:Conn_01x24", 60, 110, value="GDEY029T94 FPC", footprint=ZIF_FP,
                  fields=fields("FH12-24S-0.5SH(55)", Note="bottom contact; FPC folded 180 deg under the glass"))
    for p, n in FPC_PINS.items():
        sh.net(j1, p, n, stub=2)
    sh.text("TSCL/TSDA (6/7) open: internal temperature sensor. BS1 (8) low: 4-wire SPI. VPP (19) open, OTP only. VDD (18) is the internal 1.8 V LDO output, cap only.", (20, 175), 1.27)

    # --- J2 cable to carrier ------------------------------------------
    j2 = sh.place("J2", "Connector_Generic:Conn_01x08", 60, 220, value="CARRIER", footprint=SH8,
                  fields=fields("SM08B-SRSS-TB", Note="to carrier J3, 1:1 8-way SH cable"))
    for p, n in enumerate(["3V3_EPD", "GND", "EPD_SCK", "EPD_MOSI", "EPD_CS", "EPD_DC", "EPD_RST", "EPD_BUSY"], 1):
        sh.net(j2, p, n, stub=2)
    sh.pwr_flag("3V3_EPD", (200, 30))
    sh.pwr_flag("GND", (210, 30))

    # --- pull-ups and decoupling ----------------------------------------
    grp("J1")
    g = Grid(130, 60, 12.7, 20.32, 4)
    res(sh, cnt, "10k", "EPD_RST", "3V3_EPD", g.next(), mpn="0603WAF1002T5E")
    res(sh, cnt, "10k", "EPD_CS", "3V3_EPD", g.next(), mpn="0603WAF1002T5E")
    cap(sh, cnt, "10uF", "3V3_EPD", "GND", g.next(), mpn="CL10A106KP8NNNC", fields={"Note": "VCI"})
    cap(sh, cnt, "1uF", "3V3_EPD", "GND", g.next(), mpn="CL10A105KA8NNNC", fields={"Note": "VCI"})
    cap(sh, cnt, "1uF", "EPD_VDD", "GND", g.next(), mpn="CL10A105KA8NNNC", fields={"Note": "VDD 1.8 V LDO out"})
    cap(sh, cnt, "1uF/25V", "EPD_VSH1", "GND", g.next(), mpn="CL10A105KA8NNNC")
    cap(sh, cnt, "1uF/25V", "EPD_VSH2", "GND", g.next(), mpn="CL10A105KA8NNNC")
    cap(sh, cnt, "1uF/25V", "EPD_VSL", "GND", g.next(), mpn="CL10A105KA8NNNC")
    cap(sh, cnt, "1uF/25V", "EPD_VCOM", "GND", g.next(), mpn="CL10A105KA8NNNC")

    # --- boost / charge pump --------------------------------------------
    grp("BOOST")
    sh.text("Boost: 3V3_EPD -> L1 -> SW node; Q5 (gate GDR) switches SW to GND through R4 (RESE sense). D3 SW->VGH into C3. C5 flies SW->PUMP; D2 clamps PUMP to GND (anode PUMP); D1 pumps PUMP into VGL (anode VGL) with C4.", (220, 20), 1.5, True)
    g = Grid(240, 60, 15.24, 20.32, 5)
    cap(sh, cnt, "4.7uF/25V", "3V3_EPD", "GND", g.next(), fp=C0805, mpn="CL21B475KAQNNNE", fields={"Note": "boost input"})
    l1 = sh.place("L1", "Device:L", *g.next(), value="47uH", footprint="Inductor_SMD:L_Sunlord_SWPA3015S",
                  fields=fields("SWPA3015S470MT", Note="47uH 3.0x3.0x1.5 mm; TDK VLS3015ET-470M alt; >=0.4 A Isat"))
    sh.net(l1, 1, "3V3_EPD"); sh.net(l1, 2, "EPD_SW")
    q5 = sh.place("Q5", "Transistor_FET:AO3400A", 290, 110, value="AO3400A", footprint="Package_TO_SOT_SMD:SOT-23",
                  fields=fields("AO3400A", Note="or Si1308EDL per Good Display; Vds >= 30 V, logic level"))
    sh.net(q5, 1, "EPD_GDR"); sh.net(q5, 2, "EPD_RESE"); sh.net(q5, 3, "EPD_SW")
    r4 = res(sh, cnt, "2.2R", "EPD_RESE", "GND", (320, 115), ref="R4", mpn="0603WAF2R20T5E",
             fields={"Note": "RESE sense per GDEY029T94 reference"})
    res(sh, cnt, "1M", "EPD_GDR", "GND", (335, 115), mpn="0603WAF1004T5E", fields={"Note": "GDR pull-down"})
    d3 = sh.place("D3", "Diode:MBR0530", 265, 150, value="MBR0530", footprint="Diode_SMD:D_SOD-123", fields=fields("MBR0530"))
    sh.net(d3, 2, "EPD_SW"); sh.net(d3, 1, "EPD_VGH")  # anode SW, cathode VGH
    c3 = cap(sh, cnt, "1uF/25V", "EPD_VGH", "GND", (290, 150), ref="C3", mpn="CL10A105KA8NNNC")
    c5 = cap(sh, cnt, "4.7uF/25V", "EPD_SW", "EPD_PUMP", (320, 150), ref="C5", fp=C0805, mpn="CL21B475KAQNNNE",
             fields={"Note": "flying cap"})
    d2 = sh.place("D2", "Diode:MBR0530", 265, 185, value="MBR0530", footprint="Diode_SMD:D_SOD-123", fields=fields("MBR0530"))
    sh.net(d2, 2, "EPD_PUMP"); sh.net(d2, 1, "GND")  # anode PUMP, cathode GND
    d1 = sh.place("D1", "Diode:MBR0530", 265, 215, value="MBR0530", footprint="Diode_SMD:D_SOD-123", fields=fields("MBR0530"))
    sh.net(d1, 2, "EPD_VGL"); sh.net(d1, 1, "EPD_PUMP")  # anode VGL, cathode PUMP
    c4 = cap(sh, cnt, "1uF/25V", "EPD_VGL", "GND", (290, 215), ref="C4", mpn="CL10A105KA8NNNC")
    sh.text("Layout: L1-Q5-R4-GND and L1-D3-C3-GND loops under 10 mm; RESE a short guarded trace; the 25 V caps in a row behind J1 pins 18-24; ground pour under the cluster.", (220, 245), 1.27)
    sh.text("The brief's 10 uH / 3-4.7 R values were placeholders; the GDEY029T94 sheet specifies 47 uH 500 mA and 2.2 R. Verify against the sheet for the exact panel ordered.", (220, 250), 1.27)
    return prj

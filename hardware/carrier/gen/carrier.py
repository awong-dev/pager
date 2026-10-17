"""pager-carrier schematic: five hierarchical sheets.

Nets follow docs: firmware main/pins.h names for the existing signals and
the carrier brief's GPIO table for the new ones."""

from sch import Project
from lcsc import lookup

R_FP = "Resistor_SMD:R_0603_1608Metric"
C_FP = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C1210 = "Capacitor_SMD:C_1210_3225Metric"
SH2 = "Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal"
SH8 = "Connector_JST:JST_SH_SM08B-SRSS-TB_1x08-1MP_P1.00mm_Horizontal"
PH2 = "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal"
PH4 = "Connector_JST:JST_PH_S4B-PH-SM4-TB_1x04-1MP_P2.00mm_Horizontal"
TP_FP = "TestPoint:TestPoint_Pad_1.0x1.0mm"

DNP = {"DNP": "DNP"}
G = {"cur": None}


def grp(name):
    """Tag every passive placed from here on with a PCB placement group."""
    G["cur"] = name


class Grid:
    """Hands out positions in a row-major grid of cells."""

    def __init__(self, x0, y0, dx, dy, cols):
        self.x0, self.y0, self.dx, self.dy, self.cols = x0, y0, dx, dy, cols
        self.i = 0

    def next(self):
        c, r = self.i % self.cols, self.i // self.cols
        self.i += 1
        return self.x0 + c * self.dx, self.y0 + r * self.dy


class Counter:
    def __init__(self):
        self.n = {}
        self.used = set()

    def ref(self, prefix):
        while True:
            self.n[prefix] = self.n.get(prefix, 0) + 1
            r = "%s%d" % (prefix, self.n[prefix])
            if r not in self.used:
                self.used.add(r)
                return r

    def use(self, ref):
        self.used.add(ref)

    def reserve(self, prefix, upto):
        self.n[prefix] = max(self.n.get(prefix, 0), upto)


def two_pin(sh, cnt, prefix, lib, value, fp, net1, net2, at, fields=None, dnp=False, ref=None, mpn=None):
    f = {"MPN": mpn or "", "LCSC": lookup(mpn or ""), "DNP": "DNP" if dnp else ""}
    if fields:
        f.update(fields)
    if ref:
        cnt.use(ref)
    r = ref or cnt.ref(prefix)
    inst = sh.place(r, lib, at[0], at[1], value=value, footprint=fp, fields=f, dnp=dnp)
    inst.group = G["cur"]
    sh.net(inst, 1, net1)
    sh.net(inst, 2, net2)
    return inst


def res(sh, cnt, value, net1, net2, at, **kw):
    return two_pin(sh, cnt, "R", "Device:R", value, R_FP, net1, net2, at, **kw)


def cap(sh, cnt, value, net1, net2, at, fp=C_FP, **kw):
    return two_pin(sh, cnt, "C", "Device:C", value, fp, net1, net2, at, **kw)


def fields(mpn, dnp=False, **extra):
    f = {"MPN": mpn, "LCSC": lookup(mpn), "DNP": "DNP" if dnp else ""}
    f.update(extra)
    return f


def build(prj, lib):
    cnt = Counter()
    cnt.reserve("Q", 5)  # Q1-Q5 fixed by the brief (Q5 lives on the panel board)
    cnt.reserve("D", 5)
    cnt.reserve("L", 2)
    cnt.reserve("U", 10)
    cnt.reserve("J", 7)
    power_sheet(prj, lib, cnt)
    walter_sheet(prj, lib, cnt)
    usb_serial_sheet(prj, lib, cnt)
    display_sheet(prj, lib, cnt)
    peripherals_sheet(prj, lib, cnt)
    return prj


# ---------------------------------------------------------------------------
def power_sheet(prj, lib, cnt):
    sh = prj.add_sheet("power", "power.kicad_sch", paper="A3",
                       title="USB-C input, BQ25185 charger, battery switch, 3.3 V rails")
    sh.text("USB-C: vertical GCT USB4235-03-C (J1), 6.5 mm tall. CC1/CC2 5.1k to GND (UFP).\nD+/D- A and B pads joined at the connector, through the TVS D4 to the bridge U5.", (20, 18), 1.5, True)

    # --- J1 USB-C -------------------------------------------------------
    grp("J1")
    j1 = sh.place("J1", "Connector:USB_C_Receptacle_USB2.0_16P", 45, 65, value="USB4235",
                  footprint="pager-carrier:USB_C_Receptacle_GCT_USB4235_Vertical",
                  fields=fields("USB4235-03-C", Note="vertical 6.5 mm; fallback Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal"))
    for p, n in (("A4", "VBUS"), ("A5", "USB_CC1"), ("B5", "USB_CC2"), ("A6", "USB_DP"), ("B6", "USB_DP"),
                 ("A7", "USB_DM"), ("B7", "USB_DM"), ("A8", "NC"), ("B8", "NC"), ("A1", "GND"), ("SH", "GND")):
        sh.net(j1, p, n)
    for p in ("A9", "B4", "B9", "A12", "B1", "B12"):
        # stacked hidden pins share the same net as their visible twin; label them anyway
        sh.net(j1, p, "VBUS" if p in ("A9", "B4", "B9") else "GND", stub=0)
    g = Grid(85, 35, 12.7, 20.32, 4)
    res(sh, cnt, "5.1k", "USB_CC1", "GND", g.next(), mpn="0603WAF5101T5E")
    res(sh, cnt, "5.1k", "USB_CC2", "GND", g.next(), mpn="0603WAF5101T5E")
    sh.pwr_flag("VBUS", (260, 25))
    sh.pwr_flag("GND", (280, 25))

    # --- D4 TVS -----------------------------------------------------------
    d4 = sh.place("D4", "Power_Protection:USBLC6-2SC6", 90, 85, value="USBLC6-2SC6",
                  footprint="Package_TO_SOT_SMD:SOT-23-6", fields=fields("USBLC6-2SC6"))
    sh.net(d4, 1, "USB_DP"); sh.net(d4, 6, "USB_DP")
    sh.net(d4, 3, "USB_DM"); sh.net(d4, 4, "USB_DM")
    sh.net(d4, 5, "VBUS"); sh.net(d4, 2, "GND")
    sh.text("D4: flow-through TVS. Connector pads into pins 1/3, out of pins 6/4 toward U5.", (70, 100), 1.27)

    # --- VBUS detect -----------------------------------------------------
    grp("VBUSDET")
    g = Grid(240, 50, 12.7, 20.32, 4)
    res(sh, cnt, "56k", "VBUS", "VBUS_DET", g.next(), mpn="0603WAF5602T5E")
    res(sh, cnt, "100k", "VBUS_DET", "GND", g.next(), mpn="0603WAF1003T5E")
    cap(sh, cnt, "100nF", "VBUS_DET", "GND", g.next(), mpn="CL10B104KB8NNNC")
    sh.text("VBUS detect: 56k/100k divider -> VBUS_DET (IO15, 3.2 V at 5 V); also enables U6.", (230, 40), 1.5, True)

    # --- U1 charger -----------------------------------------------------
    grp("U1")
    sh.text("Charger BQ25185DLHR: ISET 604R = 497 mA fast charge. ILIM/VSET 18k = 500 mA input limit, 4.20 V (13k DNP = 1.1 A).\nTS unused: 10k to GND. CE is active-low: 0R to GND. STAT1/2 open-drain, 10k to 3V3.", (20, 118), 1.5, True)
    u1 = sh.place("U1", "pager-carrier:BQ25185DLHR", 50, 160, lib_path=lib,
                  fields=fields("BQ25185DLHR", LCSC="C19725033"))
    sh.net(u1, 10, "CHG_IN"); sh.net(u1, 4, "CHG_CE"); sh.net(u1, 6, "CHG_TS")
    sh.net(u1, 7, "CHG_ILIM"); sh.net(u1, 8, "CHG_ISET"); sh.net(u1, 5, "GND")
    sh.net(u1, 1, "SYS"); sh.net(u1, 2, "VBAT"); sh.net(u1, 9, "CHG_STAT1"); sh.net(u1, 3, "CHG_STAT2")
    sh.net(u1, 11, "GND")
    g = Grid(95, 140, 12.7, 20.32, 7)
    res(sh, cnt, "0R", "VBUS", "CHG_IN", g.next(), mpn="0603WAF0000T5E", fields={"Note": "R-VBUS: lift for bench tests"})
    cap(sh, cnt, "10uF/25V", "CHG_IN", "GND", g.next(), fp=C0805, mpn="CL21A106KAYNNNE")
    cap(sh, cnt, "1uF/25V", "CHG_IN", "GND", g.next(), mpn="CL10A105KA8NNNC")
    cap(sh, cnt, "22uF", "SYS", "GND", g.next(), fp=C0805, mpn="CL21A226MAQNNNE")
    cap(sh, cnt, "10uF", "VBAT", "GND", g.next(), mpn="CL10A106KP8NNNC")
    res(sh, cnt, "0R", "CHG_CE", "GND", g.next(), mpn="0603WAF0000T5E")
    res(sh, cnt, "0R", "CHG_CE", "TP_IO5", g.next(), mpn="0603WAF0000T5E", dnp=True, fields={"Note": "alt: software CE from IO5"})
    res(sh, cnt, "10k", "CHG_TS", "GND", g.next(), mpn="0603WAF1002T5E", fields={"Note": "TS not used, per datasheet"})
    res(sh, cnt, "18k", "CHG_ILIM", "GND", g.next(), mpn="0603WAF1802T5E", fields={"Note": "ILIM 500 mA, VBATREG 4.20 V"})
    res(sh, cnt, "13k", "CHG_ILIM", "GND", g.next(), mpn="0603WAF1302T5E", dnp=True, fields={"Note": "alt: ILIM 1.1 A, 4.20 V (fit instead of 18k)"})
    res(sh, cnt, "604R", "CHG_ISET", "GND", g.next(), mpn="0603WAF6040T5E", fields={"Note": "ICHG = 300/R = 497 mA (600R is not an E96 value)"})
    res(sh, cnt, "10k", "CHG_STAT1", "3V3", g.next(), mpn="0603WAF1002T5E")
    res(sh, cnt, "10k", "CHG_STAT2", "3V3", g.next(), mpn="0603WAF1002T5E")
    sh.pwr_flag("CHG_IN", (300, 25))
    sh.pwr_flag("VBAT", (320, 25))
    sh.pwr_flag("3V3", (400, 25))
    sh.text("SYS is driven by U1 SYS (power output): no PWR_FLAG. 3V3 is only sensed by U3 VOS, so it carries one.", (255, 40), 1.27)

    # --- J2 battery + F1 ------------------------------------------------
    grp("J2")
    j2 = sh.place("J2", "Connector_Generic:Conn_01x02", 30, 215, value="BATTERY", footprint=PH2,
                  fields=fields("S2B-PH-SM4-TB", Note="pin 1 = +, verify red wire on the Adafruit 328"))
    sh.net(j2, 1, "BAT_CELL"); sh.net(j2, 2, "GND")
    f1 = sh.place("F1", "Device:Polyfuse", 60, 215, rot=90, value="2A PTC", footprint="Fuse:Fuse_1206_3216Metric",
                  fields=fields("1206L200/16SR"))
    sh.net(f1, 1, "BAT_CELL"); sh.net(f1, 2, "VBAT")
    sh.pwr_flag("BAT_CELL", (340, 25))

    # --- battery switch ---------------------------------------------------
    grp("Q1")
    sh.text("Battery: J2 (pin 1 = +) through F1 2 A PTC to VBAT. Switch: SW1 drives only the Q1 gate; SYS_SW = SYS when on.\n100k gate pull-up = off when floating; 10k + 22nF soft-start limits inrush into the Walter bulk caps.", (20, 232), 1.5, True)
    sw1 = sh.place("SW1", "Switch:SW_SPDT", 40, 262, value="JS102011SAQN",
                   footprint="Button_Switch_SMD:SW_SPDT_CK_JS102011SAQN", fields=fields("JS102011SAQN"))
    sh.net(sw1, 2, "SW_COM"); sh.net(sw1, 1, "GND"); sh.net(sw1, 3, "SYS")
    sh.text("SW1: common (pin 2) -> SW_COM. Pin 1 -> GND = ON. Pin 3 -> SYS = OFF.", (20, 278), 1.27)
    q1 = sh.place("Q1", "Transistor_FET:AO3401A", 100, 262, value="DMG2305UX-7",
                  footprint="Package_TO_SOT_SMD:SOT-23", fields=fields("DMG2305UX-7", Note="or AO3401A"))
    sh.net(q1, 1, "PWR_GATE"); sh.net(q1, 2, "SYS"); sh.net(q1, 3, "SYS_SW")
    g = Grid(130, 252, 12.7, 20.32, 4)
    res(sh, cnt, "10k", "SW_COM", "PWR_GATE", g.next(), mpn="0603WAF1002T5E")
    res(sh, cnt, "100k", "PWR_GATE", "SYS", g.next(), mpn="0603WAF1003T5E")
    cap(sh, cnt, "22nF", "PWR_GATE", "SYS", g.next(), mpn="CL10B223KB8NNNC")
    sh.pwr_flag("SYS_SW", (360, 25))

    # --- U3 3.3 V buck ---------------------------------------------------
    grp("U3")
    sh.text("3.3 V always-on rail: TPS62840DLCR buck from SYS_SW, 2.2uH, 10uF in/out. EN from SYS_SW via 100k. VSET 267k = 3.3 V.\nMODE low = power-save (DNP 0R to 3V3 tries forced PWM if the e-ink shows ripple). STOP low. VOS senses 3V3.", (230, 78), 1.5, True)
    u3 = sh.place("U3", "pager-carrier:TPS62840DLCR", 255, 118, lib_path=lib, fields=fields("TPS62840DLCR"))
    sh.net(u3.sym.pin_by_name("VIN").number and u3, u3.sym.pin_by_name("VIN").number, "SYS_SW")
    sh.net(u3, u3.sym.pin_by_name("EN").number, "BUCK_EN")
    sh.net(u3, u3.sym.pin_by_name("MODE").number, "BUCK_MODE")
    sh.net(u3, u3.sym.pin_by_name("VSET").number, "BUCK_VSET")
    sh.net(u3, u3.sym.pin_by_name("STOP").number, "GND")
    sh.net(u3, u3.sym.pin_by_name("GND").number, "GND")
    sh.net(u3, u3.sym.pin_by_name("SW").number, "BUCK_SW")
    sh.net(u3, u3.sym.pin_by_name("VOS").number, "3V3")
    g = Grid(300, 100, 12.7, 20.32, 6)
    cap(sh, cnt, "10uF", "SYS_SW", "GND", g.next(), mpn="CL10A106KP8NNNC")
    l2 = sh.place("L2", "Device:L", *g.next(), value="2.2uH", footprint="Inductor_SMD:L_Murata_DFE201610P",
                  fields=fields("TFM201610ALMA-2R2MTAA", Note=">=1 A Isat, 1.0 mm tall; DFE201612E-2R2M alt"))
    sh.net(l2, 1, "BUCK_SW"); sh.net(l2, 2, "3V3")
    cap(sh, cnt, "10uF", "3V3", "GND", g.next(), mpn="CL10A106KP8NNNC")
    res(sh, cnt, "100k", "SYS_SW", "BUCK_EN", g.next(), mpn="0603WAF1003T5E")
    res(sh, cnt, "0R", "BUCK_MODE", "GND", g.next(), mpn="0603WAF0000T5E", fields={"Note": "power-save mode"})
    res(sh, cnt, "0R", "BUCK_MODE", "3V3", g.next(), mpn="0603WAF0000T5E", dnp=True, fields={"Note": "alt: forced PWM"})
    res(sh, cnt, "267k", "BUCK_VSET", "GND", g.next(), mpn="RM06FTN2673", fields={"Note": "RSET 267k = 3.3 V out (TPS62840 Table 8-2)"})

    # --- U4 display load switch -----------------------------------------
    grp("U4")
    sh.text("Display rail: U4 TPS22917 load switch 3V3 -> 3V3_EPD, ON = EPD_ENA (IO12), CT 1nF (~1 ms rise).\nR-EPD 0R in series to measure panel standby current. QOD open.", (230, 160), 1.5, True)
    u4 = sh.place("U4", "Power_Management:TPS22917DBV", 255, 190, fields=fields("TPS22917DBVR"))
    sh.net(u4, 1, "3V3"); sh.net(u4, 2, "GND"); sh.net(u4, 3, "EPD_ENA"); sh.net(u4, 4, "EPD_CT")
    sh.net(u4, 5, "NC"); sh.net(u4, 6, "EPD_SW_OUT")
    sh.text("QOD left open (no quick discharge); tie to VOUT if a fast panel power-down is wanted.", (230, 205), 1.27)
    g = Grid(300, 180, 12.7, 20.32, 4)
    cap(sh, cnt, "1nF", "EPD_CT", "GND", g.next(), mpn="CL10B102KB8NNNC")
    res(sh, cnt, "0R", "EPD_SW_OUT", "3V3_EPD", g.next(), mpn="0603WAF0000T5E", fields={"Note": "R-EPD"})
    cap(sh, cnt, "1uF", "3V3_EPD", "GND", g.next(), mpn="CL10A105KA8NNNC")
    sh.pwr_flag("3V3_EPD", (380, 25))

    # --- U10 fuel gauge ---------------------------------------------------
    grp("U10")
    sh.text("Fuel gauge U10 MAX17048 on VBAT (before the switch: keeps tracking while off, ~3 uA). I2C1 (ACC_*), addr 0x36. QSTRT and CTG low.", (230, 222), 1.5, True)
    u10 = sh.place("U10", "pager-carrier:MAX17048G+T10", 255, 250, lib_path=lib, fields=fields("MAX17048G+T10"))
    S = u10.sym
    sh.net(u10, S.pin_by_name("VDD").number, "VBAT")
    sh.net(u10, S.pin_by_name("CELL").number, "VBAT")
    sh.net(u10, S.pin_by_name("GND").number, "GND")
    sh.net(u10, S.pin_by_name("SCL").number, "ACC_SCL")
    sh.net(u10, S.pin_by_name("SDA").number, "ACC_SDA")
    sh.net(u10, S.pin_by_name("~{ALRT}").number, "NC")
    sh.net(u10, S.pin_by_name("QSTRT").number, "GND")
    sh.net(u10, S.pin_by_name("CTG").number, "GND")
    sh.net(u10, S.pin_by_name("EP").number, "GND")
    g = Grid(300, 240, 12.7, 20.32, 4)
    cap(sh, cnt, "100nF", "VBAT", "GND", g.next(), mpn="CL10B104KB8NNNC")
    return sh


# ---------------------------------------------------------------------------
WALTER_NETS = {
    1: "ESP_EN", 2: "ESP_RX0", 3: "ESP_TX0", 4: "KBD_PWR_N", 5: "EPD_ENA", 6: "EPD_BUSY", 7: "EPD_RST",
    8: "CHG_STAT1", 9: "EPD_DC", 10: "EPD_CS", 11: "EPD_MOSI", 12: "CHG_STAT2", 13: "EPD_SCK", 14: "TP_IO1",
    15: "TP_IO4", 16: "TP_IO5", 17: "HAPTIC_EN", 18: "FL_PWM", 19: "VBUS_DET", 20: "ACC_INT1", 21: "ACC_SDA",
    22: "ACC_SCL", 23: "MSG_LED", 24: "KBD_SCL", 25: "KBD_SDA", 26: "3V3_OUT", 27: "GND", 28: "SYS_SW",
}

TEST_PADS = ["GND", "GND", "SYS", "SYS_SW", "VBAT", "3V3", "3V3_EPD", "ESP_TX0", "ESP_RX0", "ESP_EN",
             "KBD_PWR_N", "TP_IO1", "TP_IO4", "TP_IO5"]


def walter_sheet(prj, lib, cnt):
    sh = prj.add_sheet("walter", "walter.kicad_sch", paper="A3",
                       title="Walter module header, bulk capacitance, test pads, mounting holes")
    sh.text("Walter 2x14 header. Pin names as datasheet rev 1.1 Table 1. The module is mounted header-side-up under the carrier; the footprint is the mirror of the vendor top-mount pattern.", (20, 20), 1.5, True)
    grp("M1")
    m1 = sh.place("M1", "pager-carrier:Walter", 80, 90, lib_path=lib, fields=fields("Walter (DPTechnics)"))
    for pin, net in WALTER_NETS.items():
        sh.net(m1, pin, net, stub=2)
    g = Grid(150, 50, 12.7, 20.32, 4)
    cap(sh, cnt, "100uF", "SYS_SW", "GND", g.next(), fp=C1210, mpn="CL32A107MQVNNNE",
        fields={"Note": "Walter VIN bulk; LTE burst"})
    cap(sh, cnt, "10uF", "SYS_SW", "GND", g.next(), mpn="CL10A106KP8NNNC")
    sh.text("Bulk at Walter VIN (pin 28): 100uF 1210 + 10uF 0603, within 5 mm of the pin.", (150, 85), 1.27)
    # test pads
    sh.text("Test pads, 1.0 mm, top side.", (150, 110), 1.5, True)
    g = Grid(150, 125, 12.7, 15.24, 7)
    for i, net in enumerate(TEST_PADS):
        tp = sh.place("TP%d" % (i + 1), "Connector:TestPoint", *g.next(), value=net, footprint=TP_FP,
                      fields=fields("", Note="test pad"))
        sh.net(tp, 1, net)
    # mounting holes
    g = Grid(150, 170, 15.24, 15.24, 4)
    for i in range(4):
        sh.place("H%d" % (i + 1), "Mechanical:MountingHole", *g.next(), value="M2",
                 footprint="MountingHole:MountingHole_2.2mm_M2", fields=fields("", Note="M2 mounting hole"))
    sh.text("IO0 (pin 4, KBD_PWR_N) is a strap, the keyboard-rail gate and the auto-reset boot target; see usb_serial.", (20, 190), 1.27)
    sh.text("Spare IO1/IO4/IO5 go to test pads only (TP_IO1 = ADC1_CH0). IO5 also has a DNP 0R to the charger CE.", (20, 195), 1.27)
    return sh


# ---------------------------------------------------------------------------
def usb_serial_sheet(prj, lib, cnt, cp2102n="GQFN24"):
    sh = prj.add_sheet("usb_serial", "usb_serial.kicad_sch", paper="A3",
                       title="CP2102N USB-UART bridge, isolation buffer, auto-reset")
    sh.text("USB-serial bridge: bus-powered CP2102N (unpowered without a cable), SN74LVC2G126 isolates UART0 when VBUS_DET is low, Espressif two-transistor auto-reset.", (20, 20), 1.5, True)
    grp("U5")
    if cp2102n == "GQFN20":
        u5 = sh.place("U5", "Interface_USB:CP2102N-Axx-xQFN20", 70, 90, value="CP2102N-A02-GQFN20",
                      footprint="Package_DFN_QFN:QFN-20-1EP_3x3mm_P0.5mm_EP1.8x1.8mm", fields=fields("CP2102N-A02-GQFN20"))
        pins = {"VREGIN": 7, "VDD": 6, "VBUS": 8, "D+": 4, "D-": 5, "RST": 9, "TXD": 18, "RXD": 17, "RTS": 16,
                "CTS": 15, "GND": 3, "EP": 21}
        nc = [1, 2, 10, 11, 13, 14, 19, 20]
        dtr = None
    else:
        u5 = sh.place("U5", "Interface_USB:CP2102N-Axx-xQFN24", 70, 90, value="CP2102N-A02-GQFN24",
                      footprint="Package_DFN_QFN:QFN-24-1EP_4x4mm_P0.5mm_EP2.6x2.6mm", fields=fields("CP2102N-A02-GQFN24"))
        S = u5.sym
        pins = {k: S.pin_by_name(n).number for k, n in (
            ("VREGIN", "VREGIN"), ("VDD", "VDD"), ("VBUS", "VBUS"), ("D+", "D+"), ("D-", "D-"), ("RST", "~{RST}"),
            ("TXD", "TXD"), ("RXD", "RXD"), ("RTS", "~{RTS}"), ("CTS", "~{CTS}"), ("DTR", "~{DTR}"), ("GND", "GND"))}
        pins["EP"] = [p.number for p in S.pins if p.name in ("GND",) and p.number != pins["GND"]]
        pins["VIO"] = S.pin_by_name("VIO").number
        dtr = pins["DTR"]
        used = set(v for v in pins.values() if isinstance(v, str)) | set(pins["EP"])
        nc = [p.number for p in S.pins if p.number not in used]
    sh.net(u5, pins["VREGIN"], "VBUS"); sh.net(u5, pins["VDD"], "BRG_VDD"); sh.net(u5, pins["VBUS"], "BRG_VBUS_SNS")
    sh.net(u5, pins["D+"], "USB_DP"); sh.net(u5, pins["D-"], "USB_DM"); sh.net(u5, pins["RST"], "BRG_RST")
    sh.net(u5, pins["TXD"], "BRG_TXD"); sh.net(u5, pins["RXD"], "BRG_RXD"); sh.net(u5, pins["RTS"], "BRG_RTS")
    sh.net(u5, pins["CTS"], "GND"); sh.net(u5, pins["GND"], "GND")
    for p in (pins["EP"] if isinstance(pins["EP"], list) else [pins["EP"]]):
        sh.net(u5, p, "GND", stub=0)
    if dtr:
        sh.net(u5, dtr, "BRG_DTR")
    if "VIO" in pins:
        sh.net(u5, pins["VIO"], "BRG_VDD")
    for p in nc:
        sh.net(u5, p, "NC")
    g = Grid(130, 40, 12.7, 20.32, 6)
    cap(sh, cnt, "4.7uF", "VBUS", "GND", g.next(), mpn="CL10A475KP8NNNC")
    cap(sh, cnt, "100nF", "VBUS", "GND", g.next(), mpn="CL10B104KB8NNNC")
    cap(sh, cnt, "4.7uF", "BRG_VDD", "GND", g.next(), mpn="CL10A475KP8NNNC")
    cap(sh, cnt, "100nF", "BRG_VDD", "GND", g.next(), mpn="CL10B104KB8NNNC")
    res(sh, cnt, "1k", "BRG_RST", "BRG_VDD", g.next(), mpn="0603WAF1001T5E", fields={"Note": "RSTb pull-up per CP2102N datasheet"})
    res(sh, cnt, "22.1k", "VBUS", "BRG_VBUS_SNS", g.next(), mpn="RC0603FR-0722K1L", fields={"Note": "VBUS sense divider"})
    res(sh, cnt, "47.5k", "BRG_VBUS_SNS", "GND", g.next(), mpn="0603WAF4752T5E")
    res(sh, cnt, "100k", "BRG_DTR", "GND", g.next(), mpn="0603WAF1003T5E")
    res(sh, cnt, "100k", "BRG_RTS", "GND", g.next(), mpn="0603WAF1003T5E")
    sh.pwr_flag("BRG_VDD", (200, 30))
    sh.text("Bus-powered: VREGIN from VBUS, VBUS sense pin via 22.1k/47.5k, VDD = internal 3.3 V regulator output, VIO tied to VDD, 4.7uF + 100nF per power pin.\nProgram the USB product string 'Pager' with Simplicity Studio Xpress Configurator after assembly.", (130, 88), 1.27)

    # --- U6 isolation buffer --------------------------------------------
    grp("U6")
    sh.text("U6 SN74LVC2G126: both OE from VBUS_DET. A: ESP_TX0 -> BRG_RXD. B: BRG_TXD -> ESP_RX0. 10k keeps ESP_RX0 high with no cable.", (20, 150), 1.5, True)
    u6a = sh.place("U6", "74xGxx:74LVC2G126", 60, 180, unit=1, value="SN74LVC2G126DCUR",
                   footprint="Package_SO:VSSOP-8_2.3x2mm_P0.5mm", fields=fields("SN74LVC2G126DCUR"))
    sh.net(u6a, 1, "VBUS_DET"); sh.net(u6a, 2, "ESP_TX0"); sh.net(u6a, 6, "BRG_RXD")
    u6b = sh.place("U6", "74xGxx:74LVC2G126", 60, 210, unit=2, value="SN74LVC2G126DCUR",
                   footprint="Package_SO:VSSOP-8_2.3x2mm_P0.5mm", fields=fields("SN74LVC2G126DCUR"))
    sh.net(u6b, 7, "VBUS_DET"); sh.net(u6b, 5, "BRG_TXD"); sh.net(u6b, 3, "ESP_RX0")
    u6c = sh.place("U6", "74xGxx:74LVC2G126", 120, 195, unit=3, value="SN74LVC2G126DCUR",
                   footprint="Package_SO:VSSOP-8_2.3x2mm_P0.5mm", fields=fields("SN74LVC2G126DCUR"))
    sh.net(u6c, 8, "3V3"); sh.net(u6c, 4, "GND")
    g = Grid(150, 180, 12.7, 20.32, 4)
    cap(sh, cnt, "100nF", "3V3", "GND", g.next(), mpn="CL10B104KB8NNNC")
    res(sh, cnt, "10k", "ESP_RX0", "3V3", g.next(), mpn="0603WAF1002T5E")

    # --- auto-reset -------------------------------------------------------
    grp("Q2")
    sh.text("Auto-reset (Espressif): Q2 DTR->EN, Q3 RTS->IO0. JP1 (closed) in Q3's collector lets auto-boot be disabled; reset still works.", (20, 240), 1.5, True)
    q2 = sh.place("Q2", "Transistor_BJT:UMH3N", 60, 275, unit=1, value="UMH3N",
                  footprint="Package_TO_SOT_SMD:SOT-363_SC-70-6", fields=fields("UMH3N", Note="or 2x MMBT3904"))
    sh.net(q2, 2, "RST_B1"); sh.net(q2, 1, "BRG_RTS"); sh.net(q2, 6, "ESP_EN")
    q3 = sh.place("Q2", "Transistor_BJT:UMH3N", 110, 275, unit=2, value="UMH3N",
                  footprint="Package_TO_SOT_SMD:SOT-363_SC-70-6", fields=fields("UMH3N"))
    sh.net(q3, 5, "RST_B2"); sh.net(q3, 4, "BRG_DTR"); sh.net(q3, 3, "IO0_RST")
    g = Grid(150, 265, 12.7, 20.32, 4)
    res(sh, cnt, "10k", "BRG_DTR", "RST_B1", g.next(), mpn="0603WAF1002T5E")
    res(sh, cnt, "10k", "BRG_RTS", "RST_B2", g.next(), mpn="0603WAF1002T5E")
    jp1 = sh.place("JP1", "Jumper:SolderJumper_2_Bridged", 60, 310, value="auto-boot",
                   footprint="Jumper:SolderJumper-2_P1.3mm_Bridged_RoundedPad1.0x1.5mm", fields=fields("", Note="open to disable auto boot mode"))
    sh.net(jp1, 1, "IO0_RST"); sh.net(jp1, 2, "KBD_PWR_N")
    sh.text("Q2/Q3 are the two halves of one UMH3N (Q2A/Q2B); the brief's Q3 is Q2B. The Walter already pulls EN up (10k); IO0 has only the ESP32 internal pull-up.", (20, 325), 1.27)
    return sh


# ---------------------------------------------------------------------------
def display_sheet(prj, lib, cnt):
    sh = prj.add_sheet("display", "display.kicad_sch", paper="A4",
                       title="Display cable to the panel board")
    sh.text("The SSD1680 ZIF and boost/charge-pump cluster live on the separate panel board (hardware/carrier/panel). The carrier carries only the 8-pin JST SH cable connector J3.", (20, 20), 1.27, True)
    grp("J3")
    j3 = sh.place("J3", "Connector_Generic:Conn_01x08", 60, 80, value="PANEL", footprint=SH8,
                  fields=fields("SM08B-SRSS-TB", Note="to panel board J2, 1:1 cable"))
    for p, n in enumerate(["3V3_EPD", "GND", "EPD_SCK", "EPD_MOSI", "EPD_CS", "EPD_DC", "EPD_RST", "EPD_BUSY"], 1):
        sh.net(j3, p, n, stub=2)
    g = Grid(110, 70, 12.7, 20.32, 4)
    cap(sh, cnt, "10uF", "3V3_EPD", "GND", g.next(), mpn="CL10A106KP8NNNC")
    sh.text("Pinout: 1 3V3_EPD, 2 GND, 3 EPD_SCK, 4 EPD_MOSI, 5 EPD_CS, 6 EPD_DC, 7 EPD_RST, 8 EPD_BUSY. The panel board carries the RST/CS pull-ups and the VCI decoupling.", (20, 110), 1.27)
    return sh


# ---------------------------------------------------------------------------
def peripherals_sheet(prj, lib, cnt):
    sh = prj.add_sheet("peripherals", "peripherals.kicad_sch", paper="A3",
                       title="Accelerometer, keyboard, front light, message LED, haptic")
    # --- U7 accelerometer ---------------------------------------------
    grp("U7")
    sh.text("U7 LIS2DW12 on I2C1 (ACC_*), SA0 low = 0x18, CS high = I2C mode, INT1 -> IO16 (RTC wake). Always on from 3V3.", (20, 20), 1.5, True)
    u7 = sh.place("U7", "pager-carrier:LIS2DW12TR", 60, 60, lib_path=lib, fields=fields("LIS2DW12TR"))
    S = u7.sym
    for name, net in (("VDD", "3V3"), ("VDD_IO", "3V3"), ("SCL/SPC", "ACC_SCL"), ("SDA/SDI/SDO", "ACC_SDA"),
                      ("CS", "3V3"), ("SDO/SA0", "GND"), ("INT1", "ACC_INT1"), ("INT2", "NC"), ("RES", "GND"),
                      ("NC", "NC")):
        sh.net(u7, S.pin_by_name(name).number, net)
    for p in S.pins:
        if p.name == "GND":
            sh.net(u7, p.number, "GND")
    g = Grid(120, 40, 12.7, 20.32, 4)
    cap(sh, cnt, "100nF", "3V3", "GND", g.next(), mpn="CL10B104KB8NNNC")
    cap(sh, cnt, "10uF", "3V3", "GND", g.next(), mpn="CL10A106KP8NNNC")
    res(sh, cnt, "4.7k", "ACC_SDA", "3V3", g.next(), mpn="0603WAF4701T5E")
    res(sh, cnt, "4.7k", "ACC_SCL", "3V3", g.next(), mpn="0603WAF4701T5E")
    sh.text("Place U7 away from J7 and the panel cable; X axis along the tin's long edge; mark orientation on silk.", (20, 90), 1.27)

    # --- J4 keyboard ------------------------------------------------------
    grp("J4")
    sh.text("J4 CardKB (Grove order): 1 KBD_SCL, 2 KBD_SDA, 3 3V3_OUT, 4 GND. Pull-ups to 3V3_OUT so they vanish with the rail.", (20, 110), 1.5, True)
    j4 = sh.place("J4", "Connector_Generic:Conn_01x04", 60, 140, value="CARDKB", footprint=PH4,
                  fields=fields("S4B-PH-SM4-TB", Note="Grove HY2.0-4P compatible"))
    for p, n in enumerate(["KBD_SCL", "KBD_SDA", "3V3_OUT", "GND"], 1):
        sh.net(j4, p, n, stub=2)
    g = Grid(120, 125, 12.7, 20.32, 4)
    res(sh, cnt, "4.7k", "KBD_SCL", "3V3_OUT", g.next(), mpn="0603WAF4701T5E")
    res(sh, cnt, "4.7k", "KBD_SDA", "3V3_OUT", g.next(), mpn="0603WAF4701T5E")
    cap(sh, cnt, "10uF", "3V3_OUT", "GND", g.next(), mpn="CL10A106KP8NNNC")

    # --- front light ------------------------------------------------------
    grp("U8")
    sh.text("Front light: U8 20 mA CCR (anode pin 2 from SYS_SW, cathode pin 1 -> J5 pin 1); Q4 low-side switch on J5 pin 2, PWM from FL_PWM (IO7). R-FL DNP alt.\nNSI45020 needs ~1.8 V overhead: with a ~3 V white LED it only regulates above ~4.8 V on SYS_SW, i.e. on USB; on battery it passes less than 20 mA.", (20, 170), 1.5, True)
    j5 = sh.place("J5", "Connector_Generic:Conn_01x02", 60, 200, value="FRONTLIGHT", footprint=SH2,
                  fields=fields("SM02B-SRSS-TB"))
    sh.net(j5, 1, "FL_ANODE", stub=2); sh.net(j5, 2, "FL_CATHODE", stub=2)
    u8 = sh.place("U8", "pager-carrier:NSI45020AT1G", 120, 190, lib_path=lib, fields=fields("NSI45020AT1G", Note="20 mA placeholder until the accessory is chosen"))
    sh.net(u8, 2, "SYS_SW"); sh.net(u8, 1, "FL_ANODE")
    g = Grid(160, 185, 12.7, 20.32, 4)
    res(sh, cnt, "DNP", "SYS_SW", "FL_ANODE", g.next(), dnp=True, mpn="", fields={"Note": "R-FL: series resistor alternative to U8"})
    q4 = sh.place("Q4", "Transistor_FET:2N7002", 120, 225, value="2N7002", footprint="Package_TO_SOT_SMD:SOT-23",
                  fields=fields("2N7002"))
    sh.net(q4, 1, "FL_G"); sh.net(q4, 2, "GND"); sh.net(q4, 3, "FL_CATHODE")
    g = Grid(160, 215, 12.7, 20.32, 4)
    res(sh, cnt, "100R", "FL_PWM", "FL_G", g.next(), mpn="0603WAF1000T5E")
    res(sh, cnt, "100k", "FL_G", "GND", g.next(), mpn="0603WAF1003T5E")

    # --- message LED ----------------------------------------------------
    grp("D5")
    sh.text("Message LED (DNP): MSG_LED (IO8, RTC GPIO) -> 330R -> J6 pin 1; J6 pin 2 GND. D5 + 1k on-board bench LED on the same net (DNP).", (20, 250), 1.5, True)
    j6 = sh.place("J6", "Connector_Generic:Conn_01x02", 60, 280, value="MSG_LED", footprint=SH2,
                  fields=fields("SM02B-SRSS-TB", dnp=True), dnp=True)
    sh.net(j6, 1, "LED_A", stub=2); sh.net(j6, 2, "GND", stub=2)
    g = Grid(120, 270, 12.7, 20.32, 4)
    res(sh, cnt, "330R", "MSG_LED", "LED_A", g.next(), mpn="0603WAF3300T5E", dnp=True)
    res(sh, cnt, "1k", "MSG_LED", "LED_D5", g.next(), mpn="0603WAF1001T5E", dnp=True)
    d5 = sh.place("D5", "Device:LED", *g.next(), rot=90, value="RED", footprint="LED_SMD:LED_0603_1608Metric",
                  fields=fields("KT-0603R", dnp=True), dnp=True)
    sh.net(d5, 2, "LED_D5"); sh.net(d5, 1, "GND")

    # --- haptic -----------------------------------------------------------
    grp("U9")
    sh.text("Haptic (DNP): DRV2605L on I2C1 (0x5A), EN = HAPTIC_EN (IO6) with 100k pull-down, IN/TRIG to GND (I2C trigger), OUT+/- to J7.", (220, 20), 1.5, True)
    u9 = sh.place("U9", "pager-carrier:DRV2605LDGSR", 260, 60, lib_path=lib, fields=fields("DRV2605LDGSR", dnp=True), dnp=True)
    S = u9.sym
    for name, net in (("VDD", "3V3"), ("REG", "HAP_REG"), ("SCL", "ACC_SCL"), ("SDA", "ACC_SDA"), ("IN/TRIG", "GND"),
                      ("EN", "HAPTIC_EN"), ("OUT+", "HAP_OUTP"), ("OUT-", "HAP_OUTN"), ("GND", "GND")):
        sh.net(u9, S.pin_by_name(name).number, net)
    sh.net(u9, S.pin_by_name("VDD/NC").number, "3V3")
    g = Grid(310, 40, 12.7, 20.32, 4)
    cap(sh, cnt, "1uF", "3V3", "GND", g.next(), mpn="CL10A105KA8NNNC", dnp=True)
    cap(sh, cnt, "100nF", "3V3", "GND", g.next(), mpn="CL10B104KB8NNNC", dnp=True)
    cap(sh, cnt, "1uF", "HAP_REG", "GND", g.next(), mpn="CL10A105KA8NNNC", dnp=True)
    res(sh, cnt, "100k", "HAPTIC_EN", "GND", g.next(), mpn="0603WAF1003T5E", dnp=True)
    j7 = sh.place("J7", "Connector_Generic:Conn_01x02", 260, 110, value="HAPTIC", footprint=SH2,
                  fields=fields("SM02B-SRSS-TB", dnp=True), dnp=True)
    sh.net(j7, 1, "HAP_OUTP", stub=2); sh.net(j7, 2, "HAP_OUTN", stub=2)
    return sh

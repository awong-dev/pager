"""Custom symbol definitions for pager-carrier.kicad_sym.

Pin tables come from the manufacturer datasheets (see research notes in the
README). Each entry: (number, name, electrical type)."""

from customlib import box_symbol, power_symbol

POWER_RAILS = ["3V3", "VBUS", "VBAT", "SYS", "SYS_SW", "3V3_EPD", "3V3_OUT", "CHG_IN", "BAT_CELL"]

# Walter header, datasheet rev 1.1 Table 1, names exactly as printed.
WALTER_LEFT = [
    (1, "RESET", "input"),
    (2, "IO44/RX0", "bidirectional"),
    (3, "IO43/TX0", "bidirectional"),
    (4, "DFU/3V3 EN", "bidirectional"),
    (5, "IO12", "bidirectional"),
    (6, "IO11", "bidirectional"),
    (7, "IO13", "bidirectional"),
    (8, "IO38", "bidirectional"),
    (9, "IO39", "bidirectional"),
    (10, "IO40", "bidirectional"),
    (11, "IO41", "bidirectional"),
    (12, "IO42", "bidirectional"),
    (13, "IO2", "bidirectional"),
    (14, "IO1", "bidirectional"),
]
WALTER_RIGHT = [
    (28, "VIN", "power_in"),
    (27, "GND", "power_in"),
    (26, "3V3 OUT", "power_out"),
    (25, "IO10", "bidirectional"),
    (24, "IO9", "bidirectional"),
    (23, "IO8", "bidirectional"),
    (22, "IO18", "bidirectional"),
    (21, "IO17", "bidirectional"),
    (20, "IO16", "bidirectional"),
    (19, "IO15", "bidirectional"),
    (18, "IO7", "bidirectional"),
    (17, "IO6", "bidirectional"),
    (16, "IO5", "bidirectional"),
    (15, "IO4", "bidirectional"),
]

# BQ25185 (TI SLUSF65B Table 4-1): SYS 1, BAT 2, STAT2 3, CE 4, GND 5, TS/MR 6,
# ILIM/VSET 7, ISET 8, STAT1 9, IN 10, thermal pad 11.
BQ25185_LEFT = [
    (10, "IN", "power_in"),
    None,
    (4, "~{CE}", "input"),
    (6, "TS/MR", "input"),
    (7, "ILIM/VSET", "input"),
    (8, "ISET", "input"),
    None,
    (5, "GND", "power_in"),
]
BQ25185_RIGHT = [
    (1, "SYS", "power_out"),
    (2, "BAT", "passive"),
    None,
    (9, "STAT1", "open_collector"),
    (3, "STAT2", "open_collector"),
    None,
    None,
    (11, "EP", "passive"),
]

# TPS62840 DLC (VSON-HR / SOT-5X3, DLC0008B), TI SLVSEC6D: 1 GND, 2 VIN, 3 MODE,
# 4 EN, 5 VSET, 6 STOP, 7 SW, 8 VOS. Output voltage set by RSET on VSET
# (267 k = 3.3 V); there is no fixed-3.3 V orderable.
TPS62840_LEFT = [
    (2, "VIN", "power_in"),
    (4, "EN", "input"),
    (3, "MODE", "input"),
    (5, "VSET", "input"),
    (6, "STOP", "input"),
    (1, "GND", "power_in"),
]
TPS62840_RIGHT = [
    (7, "SW", "output"),
    (8, "VOS", "input"),
    None, None, None, None,
]

# LIS2DW12 LGA-12 (ST DS11624): 1 SCL/SPC, 2 CS, 3 SDO/SA0, 4 SDA/SDI/SDO,
# 5 NC, 6 GND, 7 RES, 8 GND, 9 VDD, 10 VDD_IO, 11 INT2, 12 INT1
LIS2DW12_LEFT = [
    (9, "VDD", "power_in"),
    (10, "VDD_IO", "power_in"),
    None,
    (1, "SCL/SPC", "input"),
    (4, "SDA/SDI/SDO", "bidirectional"),
    (2, "CS", "input"),
    (3, "SDO/SA0", "input"),
    None,
    (6, "GND", "power_in"),
]
LIS2DW12_RIGHT = [
    (12, "INT1", "output"),
    (11, "INT2", "output"),
    None, None, None, None,
    (7, "RES", "passive"),
    (5, "NC", "no_connect"),
    (8, "GND", "power_in"),
]

# DRV2605L DGS (VSSOP-10), TI: 1 REG, 2 SCL, 3 SDA, 4 IN/TRIG, 5 EN, 6 VDD/NC, 7 OUT+, 8 GND, 9 OUT-, 10 VDD
DRV2605L_LEFT = [
    (10, "VDD", "power_in"),
    (1, "REG", "power_out"),
    (2, "SCL", "input"),
    (3, "SDA", "bidirectional"),
    (4, "IN/TRIG", "input"),
    (5, "EN", "input"),
    (8, "GND", "power_in"),
]
DRV2605L_RIGHT = [
    (7, "OUT+", "output"),
    (9, "OUT-", "output"),
    None, None, None,
    (6, "VDD/NC", "passive"),
    None,
]

# MAX17048 TDFN-8 (T822+3): 1 CTG, 2 CELL, 3 VDD, 4 GND, 5 ALRT, 6 QSTRT, 7 SCL, 8 SDA, EP (9) GND
MAX17048_LEFT = [
    (3, "VDD", "power_in"),
    (2, "CELL", "input"),
    (1, "CTG", "passive"),
    (4, "GND", "power_in"),
]
MAX17048_RIGHT = [
    (7, "SCL", "input"),
    (8, "SDA", "bidirectional"),
    (5, "~{ALRT}", "open_collector"),
    (6, "QSTRT", "input"),
    (9, "EP", "passive"),
]


def all_symbols():
    syms = [power_symbol(n) for n in POWER_RAILS]
    syms.append(box_symbol("Walter", WALTER_LEFT, WALTER_RIGHT, width=27.94, value="Walter",
                           footprint="pager-carrier:Walter_Module_2x14_THT",
                           datasheet="https://www.quickspot.io/datasheet/walter_datasheet.pdf",
                           description="DPTechnics Walter ESP32-S3 + Sequans GM02SP LTE-M/NB-IoT module, 2x14 header",
                           keywords="walter esp32 lte"))
    syms.append(box_symbol("BQ25185DLHR", BQ25185_LEFT, BQ25185_RIGHT, width=20.32, value="BQ25185DLHR",
                           footprint="pager-carrier:BQ25185_WSON-10_DLH0010A_2.2x2mm_P0.4mm_EP0.9x1.5mm",
                           datasheet="https://www.ti.com/lit/ds/symlink/bq25185.pdf",
                           description="1-cell 1 A linear Li-ion charger with power path, WSON-10",
                           keywords="charger lipo power-path"))
    syms.append(box_symbol("TPS62840DLCR", TPS62840_LEFT, TPS62840_RIGHT, width=17.78, value="TPS62840DLCR",
                           footprint="pager-carrier:TPS62840_VSON-HR-8_DLC0008B_1.5x2mm_P0.5mm",
                           datasheet="https://www.ti.com/lit/ds/symlink/tps62840.pdf",
                           description="750 mA 60 nA Iq buck converter, VSET resistor sets 1.8-3.3 V, VSON-HR-8 (SOT-5X3)",
                           keywords="buck regulator"))
    syms.append(box_symbol("LIS2DW12TR", LIS2DW12_LEFT, LIS2DW12_RIGHT, width=22.86, value="LIS2DW12TR",
                           footprint="Package_LGA:LGA-12_2x2mm_P0.5mm",
                           datasheet="https://www.st.com/resource/en/datasheet/lis2dw12.pdf",
                           description="3-axis MEMS accelerometer, I2C/SPI, LGA-12 2x2 mm",
                           keywords="accelerometer motion"))
    syms.append(box_symbol("DRV2605LDGSR", DRV2605L_LEFT, DRV2605L_RIGHT, width=17.78, value="DRV2605LDGSR",
                           footprint="Package_SO:TSSOP-10_3x3mm_P0.5mm",
                           datasheet="https://www.ti.com/lit/ds/symlink/drv2605l.pdf",
                           description="Haptic driver for ERM/LRA with effect library, I2C, VSSOP-10",
                           keywords="haptic driver"))
    syms.append(box_symbol("MAX17048G+T10", MAX17048_LEFT, MAX17048_RIGHT, width=17.78, value="MAX17048G+T10",
                           footprint="Package_DFN_QFN:TDFN-8-1EP_2x2mm_P0.5mm_EP0.8x1.2mm",
                           datasheet="https://www.analog.com/media/en/technical-documentation/data-sheets/max17048-max17049.pdf",
                           description="ModelGauge 1-cell fuel gauge, I2C, TDFN-8 2x2 mm",
                           keywords="fuel gauge battery"))
    # Constant-current regulator, 2-pin diode-like: pin 1 anode (in), pin 2 cathode (out)
    syms.append(box_symbol("NSI45020AT1G", [(2, "A", "passive")], [(1, "K", "passive")], width=10.16,
                           value="NSI45020AT1G", footprint="Diode_SMD:D_SOD-123",
                           datasheet="https://www.onsemi.com/pdf/datasheet/nsi45020a-d.pdf",
                           description="20 mA constant-current LED regulator, SOD-123; pin 1 = cathode (band), pin 2 = anode",
                           keywords="ccr led current regulator"))
    return syms

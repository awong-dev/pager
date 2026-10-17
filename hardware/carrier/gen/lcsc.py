"""LCSC part numbers by MPN (verified on jlcpcb.com / lcsc.com, 16-17 Oct
2026). 'B' = JLCPCB basic part, 'E' = extended."""

LCSC = {
    # resistors 0603 1 % (UNI-ROYAL)
    "0603WAF0000T5E": ("C21189", "B"),
    "0603WAF1000T5E": ("C22775", "B"),
    "0603WAF3300T5E": ("C23138", "B"),
    "0603WAF6040T5E": ("C32183", "E"),  # 604R (600R is not an E96 value)
    "0603WAF1001T5E": ("C21190", "B"),
    "0603WAF4701T5E": ("C23162", "B"),
    "0603WAF5101T5E": ("C23186", "B"),
    "0603WAF1002T5E": ("C25804", "B"),
    "0603WAF1302T5E": ("C22797", "B"),
    "0603WAF1802T5E": ("C25810", "B"),
    "0603WAF5602T5E": ("C23206", "B"),
    "0603WAF1003T5E": ("C25803", "B"),
    "0603WAF1004T5E": ("C22935", "B"),
    "RM06FTN2673": ("C188011", "E"),      # 267k 1% TA-I
    "RC0603FR-0722K1L": ("C137768", "E"),  # 22.1k 1% Yageo
    "0603WAF4752T5E": ("C23061", "E"),
    "0603WAF220KT5E": ("C22939", "B"),    # 2.2R
    # capacitors
    "CC0603KRX7R9BB104": ("C14663", "B"),  # 100nF 50V X7R
    "CL10B104KB8NNNC": ("C14663", "B"),    # mapped to the basic Yageo 100nF
    "CL10B102KB8NNNC": ("C1588", "B"),
    "CL10B223KB8NNNC": ("C21122", "B"),
    "CL10A105KB8NNNC": ("C15849", "B"),    # 1uF 50V X5R 0603 basic
    "CL10A105KA8NNNC": ("C15849", "B"),    # 1uF 25V requested -> basic 50V part
    "CL10A106KP8NNNC": ("C19702", "B"),    # 10uF 10V X5R 0603
    "CL10A475KP8NNNC": ("C19666", "B"),    # 4.7uF -> basic 16V part CL10A475KO8NNNC
    "CL21A106KAYNNNE": ("C15850", "B"),    # 10uF 25V 0805
    "CL21A226MAQNNNE": ("C45783", "B"),    # 22uF 25V 0805
    "CL21B475KAQNNNE": ("C1779", "B"),     # 4.7uF 25V X5R 0805 basic (X7R C98195 extended)
    "CL32A107MQVNNNE": ("C49066", "E"),    # 100uF 6.3V 1210
    # semis
    "MBR0530": ("C82046", "E"),
    "USBLC6-2SC6": ("C7519", "E"),
    "AO3400A": ("C20917", "B"),
    "AO3401A": ("C15127", "B"),
    "DMG2305UX-7": ("C150470", "E"),
    "2N7002": ("C8545", "B"),
    "MMBT3904": ("C20526", "B"),
    "UMH3N": ("C62892", "E"),              # CJ second source; Rohm UMH3NTN = C123008
    "NSI45020AT1G": ("C128367", "E"),
    "SN74LVC2G126DCUR": ("", "-"),         # VSSOP-8 not stocked; DCTR (C183273) is MSOP-8 0.65 mm
    "TPS22917DBVR": ("C2681320", "E"),
    "TPS62840DLCR": ("C2071859", "E"),
    "CP2102N-A02-GQFN20": ("C969913", "E"),
    "CP2102N-A02-GQFN24": ("C969151", "E"),
    "CP2102N-A02-GQFN28": ("C964632", "E"),
    "LIS2DW12TR": ("C189624", "E"),
    "DRV2605LDGSR": ("C527464", "E"),
    "MAX17048G+T10": ("C2682616", "E"),
    "BQ25185DLHR": ("C19725033", "E"),
    # passives / mech
    "VLS252012HBX-100M-1": ("C88532", "E"),
    "FNR3015S470MT": ("C167766", "E"),   # cjiang 47uH 3015, 430 mA Isat
    "TFM201610ALMA-2R2MTAA": ("C404810", "E"),
    "DFE201612E-2R2M": ("C404810", "E"),   # not stocked; TFM201610ALMA2R2MTAA substituted
    "MF-NSMF200-2": ("C89656", "E"),
    "1206L200/16SR": ("C151163", "E"),     # 1206L200PR
    "JS102011SAQN": ("C221660", "E"),
    "S2B-PH-SM4-TB": ("C295747", "E"),
    "S4B-PH-SM4-TB": ("C265102", "E"),
    "SM02B-SRSS-TB": ("C160402", "E"),
    "SM08B-SRSS-TB": ("C160407", "E"),
    "FH12-24S-0.5SH(55)": ("C202112", "E"),
    "USB4235-03-C": ("", "-"),             # not stocked at JLC/LCSC; hand-place
    "USB4235-GF-A": ("", "-"),
    "KT-0603R": ("C2286", "B"),
}


def lookup(mpn):
    e = LCSC.get(mpn)
    return e[0] if e else ""

"""Builds the project symbol library pager-carrier.kicad_sym: power symbols
for the named rails plus box-style IC/module symbols that the stock library
lacks. Returns sexp nodes; symlib.load_symbol reads the written file back."""

import copy

from sexp import dump, find, find_all, q
from symlib import load_lib, KICAD_SYMBOLS

LIB_VERSION = 20251024


def _effects(size=1.27, hide=False, justify=None):
    e = ["effects", ["font", ["size", size, size]]]
    if justify:
        e.append(["justify", justify])
    if hide:
        e.append(["hide", "yes"])
    return e


def _prop(name, value, x=0, y=0, hide=False, justify=None, rot=0):
    n = ["property", q(name), q(value), ["at", x, y, rot]]
    if hide:
        n.append(["hide", "yes"])
    n.append(["effects", ["font", ["size", 1.27, 1.27]]] + ([["justify", justify]] if justify else []))
    return n


def power_symbol(net):
    """Clone power:+3V3 and rename it PWR_<net>."""
    lib = load_lib(KICAD_SYMBOLS + "/power.kicad_sym")
    node = copy.deepcopy(lib["+3V3"])
    name = "PWR_" + net
    node[1] = q(name)
    for p in find_all(node, "property"):
        if p[1] == "Value":
            p[2] = q(net)
        if p[1] == "Description":
            p[2] = q("Power symbol for the %s rail" % net)
        if p[1] == "ki_keywords":
            p[2] = q("power-flag")
    for sub in find_all(node, "symbol"):
        old = str(sub[1])
        sub[1] = q(name + old[len("+3V3"):])
        for pin in find_all(sub, "pin"):
            find(pin, "name")[1] = q(net)
    return node


def box_symbol(name, left, right, top=(), bottom=(), width=None, value=None, footprint="",
               datasheet="~", description="", keywords="", pin_len=2.54, name_offset=0.508):
    """Generic rectangular IC symbol.

    left/right/top/bottom: lists of (number, name, etype) or None for a gap.
    Pin positions are on a 2.54 grid; the body is sized to fit."""
    rows = max(len(left), len(right))
    cols = max(len(top), len(bottom))
    if width is None:
        # fit the longest names on each side
        ln = max([len(p[1]) for p in left if p] + [0])
        rn = max([len(p[1]) for p in right if p] + [0])
        width = max(12.7, (ln + rn) * 1.4 + 7.62)
        width = round(width / 2.54) * 2.54
    height = max(rows + 1, cols + 1) * 2.54
    height = max(height, 5.08)
    hw = width / 2
    hh = (rows + 1) * 2.54 / 2 if rows else height / 2
    hh = max(hh, (cols + 1) * 1.27 if cols else 0)
    # body corners
    x0, y0, x1, y1 = -hw, -hh, hw, hh
    sym = ["symbol", q(name), ["exclude_from_sim", "no"], ["in_bom", "yes"], ["on_board", "yes"],
           ["in_pos_files", "yes"],
           _prop("Reference", "U", x0, y1 + 1.27, justify="left"),
           _prop("Value", value or name, x0, y0 - 1.27, justify="left"),
           _prop("Footprint", footprint, 0, 0, hide=True),
           _prop("Datasheet", datasheet, 0, 0, hide=True),
           _prop("Description", description, 0, 0, hide=True),
           _prop("ki_keywords", keywords, 0, 0, hide=True)]
    body = ["symbol", q(name + "_0_1"),
            ["rectangle", ["start", x0, y1], ["end", x1, y0],
             ["stroke", ["width", 0.254], ["type", "default"]], ["fill", ["type", "background"]]]]
    pins_node = ["symbol", q(name + "_1_1")]

    def pin(number, pname, etype, x, y, angle):
        return ["pin", etype, "line", ["at", x, y, angle], ["length", pin_len],
                ["name", q(pname), _effects()], ["number", q(str(number)), _effects()]]

    def ys(n):
        # evenly spaced from top, 2.54 pitch, centred
        top_y = (n - 1) * 2.54 / 2
        return [round(top_y - i * 2.54, 4) for i in range(n)]

    for i, p in enumerate(left):
        if p:
            pins_node.append(pin(p[0], p[1], p[2], x0 - pin_len, ys(rows)[i], 0))
    for i, p in enumerate(right):
        if p:
            pins_node.append(pin(p[0], p[1], p[2], x1 + pin_len, ys(rows)[i], 180))

    def xs(n):
        left_x = -(n - 1) * 2.54 / 2
        return [round(left_x + i * 2.54, 4) for i in range(n)]

    for i, p in enumerate(top):
        if p:
            pins_node.append(pin(p[0], p[1], p[2], xs(cols)[i], y1 + pin_len, 270))
    for i, p in enumerate(bottom):
        if p:
            pins_node.append(pin(p[0], p[1], p[2], xs(cols)[i], y0 - pin_len, 90))
    sym.append(body)
    sym.append(pins_node)
    sym.append(["embedded_fonts", "no"])
    return sym


def write_lib(path, symbols):
    n = ["kicad_symbol_lib", ["version", LIB_VERSION], ["generator", q("pager_carrier_gen")],
         ["generator_version", q("10.0")]] + list(symbols)
    with open(path, "w", encoding="utf-8") as f:
        f.write(dump(n))

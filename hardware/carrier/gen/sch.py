"""Schematic writer: places symbols on hierarchical sheets and hooks every pin
to a global label, a power symbol or a no-connect flag.

Coordinates are schematic millimetres (y down). Symbol pins come from
symlib.Symbol in symbol space (y up); `xform` maps them to the sheet.
"""

import math
import uuid

from sexp import Quoted, dump, find, find_all, q
from symlib import load_symbol

GRID = 1.27
NS = uuid.UUID("8f2a9d3c-5b1e-4c7a-9e2d-1f0b6a7c8d9e")


def uid(*parts):
    return str(uuid.uuid5(NS, "|".join(str(p) for p in parts)))


def snap(v):
    return round(round(v / GRID) * GRID, 4)


def rot_vec(dx, dy, rot):
    """Rotate a schematic-space vector by rot degrees counter-clockwise on
    screen (KiCad's R key direction). Schematic y points down."""
    r = rot % 360
    if r == 0:
        return dx, dy
    if r == 90:
        return dy, -dx
    if r == 180:
        return -dx, -dy
    if r == 270:
        return -dy, dx
    raise ValueError(rot)


def xform(px, py, X, Y, rot, mirror=None):
    """Symbol-space pin position -> sheet position."""
    dx, dy = px, -py  # flip y
    if mirror == "x":
        dy = -dy
    elif mirror == "y":
        dx = -dx
    dx, dy = rot_vec(dx, dy, rot)
    return round(X + dx, 4), round(Y + dy, 4)


def pin_outward(pin, rot, mirror=None):
    """Unit vector pointing away from the symbol body at the pin's connection
    point, in sheet space."""
    a = pin.angle % 360
    # direction the pin extends from its connection point toward the body, symbol space (y up)
    body = {0: (1, 0), 90: (0, 1), 180: (-1, 0), 270: (0, -1)}[int(a)]
    dx, dy = body[0], -body[1]
    if mirror == "x":
        dy = -dy
    elif mirror == "y":
        dx = -dx
    dx, dy = rot_vec(dx, dy, rot)
    return -dx, -dy


POWER_NETS = {"GND", "3V3", "VBUS", "VBAT", "SYS", "SYS_SW", "3V3_EPD", "3V3_OUT"}


def power_symbol_for(net):
    if net == "GND":
        return "power:GND"
    return "pager-carrier:PWR_" + net


class Instance:
    def __init__(self, ref, sym, unit, X, Y, rot, mirror, fields, dnp, in_bom, on_board):
        self.ref = ref
        self.sym = sym
        self.unit = unit
        self.X = X
        self.Y = Y
        self.rot = rot
        self.mirror = mirror
        self.fields = fields
        self.dnp = dnp
        self.in_bom = in_bom
        self.on_board = on_board
        self.uuid = uid("sym", ref, unit)
        self.nets = {}  # pin number -> net name (or None)

    def pin_pos(self, number):
        p = self.sym.pin(number, self.unit)
        return xform(p.x, p.y, self.X, self.Y, self.rot, self.mirror)


class Sheet:
    def __init__(self, project, name, filename, paper="A4", title=""):
        self.project = project
        self.name = name
        self.filename = filename
        self.paper = paper
        self.title = title
        self.uuid = uid("sheet", filename)          # the sheet file's uuid
        self.symbol_uuid = uid("sheetsym", filename)  # the sheet symbol's uuid in the root (must differ)
        self.instances = []
        self.items = []  # extra sexp nodes (wires, labels, text)
        self.lib_symbols = {}
        self.pwr_count = 0
        self.flag_count = 0

    # ---- placement -----------------------------------------------------
    def place(self, ref, lib_id, X, Y, rot=0, mirror=None, unit=1, value=None, footprint=None,
              fields=None, dnp=False, in_bom=True, on_board=True, lib_path=None):
        sym = load_symbol(lib_id, lib_path) if isinstance(lib_id, str) else lib_id
        self.lib_symbols.setdefault(sym.lib_id, sym)
        f = {"Reference": ref,
             "Value": value if value is not None else (sym.prop("Value") or sym.base),
             "Footprint": footprint if footprint is not None else (sym.prop("Footprint") or ""),
             "Datasheet": sym.prop("Datasheet") or "~",
             "Description": sym.prop("Description") or ""}
        if fields:
            f.update(fields)
        inst = Instance(ref, sym, unit, snap(X), snap(Y), rot, mirror, f, dnp, in_bom, on_board)
        self.instances.append(inst)
        self.project.register(inst, self)
        return inst

    # ---- connections ---------------------------------------------------
    def net(self, inst, pin, net, stub=1, label=None):
        """Connect pin to net: power symbol for rails, global label otherwise,
        no-connect flag when net is None/"NC"."""
        pin = str(pin)
        p = inst.sym.pin(pin, inst.unit)
        x, y = inst.pin_pos(pin)
        ox, oy = pin_outward(p, inst.rot, inst.mirror)
        if net in (None, "NC"):
            inst.nets[pin] = None
            self.items.append(["no_connect", ["at", x, y], ["uuid", q(uid("nc", inst.ref, pin))]])
            return
        inst.nets[pin] = net
        self.project.nets.setdefault(net, []).append((inst.ref, pin))
        lx, ly = round(x + ox * stub * 2.54, 4), round(y + oy * stub * 2.54, 4)
        if stub:
            self.wire((x, y), (lx, ly))
        if net in POWER_NETS and label is None:
            self.power(net, (lx, ly), (ox, oy))
        else:
            self.glabel(net, (lx, ly), (ox, oy), shape=label or "passive")

    def wire(self, a, b):
        self.items.append(["wire", ["pts", ["xy", a[0], a[1]], ["xy", b[0], b[1]]],
                           ["stroke", ["width", 0], ["type", "default"]],
                           ["uuid", q(uid("wire", a, b, len(self.items)))]])

    def glabel(self, net, at, direction, shape="passive"):
        ox, oy = direction
        if ox > 0:
            ang, just = 0, "left"
        elif ox < 0:
            ang, just = 180, "right"
        elif oy < 0:
            ang, just = 90, "left"
        else:
            ang, just = 270, "right"
        self.items.append(["global_label", q(net), ["shape", shape], ["at", at[0], at[1], ang],
                           ["fields_autoplaced", "yes"],
                           ["effects", ["font", ["size", 1.27, 1.27]], ["justify", just]],
                           ["uuid", q(uid("gl", net, at))]])

    def label(self, net, at, direction):
        ox, oy = direction
        if ox > 0:
            ang, just = 0, "left"
        elif ox < 0:
            ang, just = 180, "right"
        elif oy < 0:
            ang, just = 90, "left"
        else:
            ang, just = 270, "right"
        self.items.append(["label", q(net), ["at", at[0], at[1], ang],
                           ["effects", ["font", ["size", 1.27, 1.27]], ["justify", just]],
                           ["uuid", q(uid("ll", net, at))]])

    def power(self, net, at, direction):
        lib_id = power_symbol_for(net)
        sym = load_symbol(lib_id, self.project.custom_lib if lib_id.startswith("pager-carrier:") else None)
        self.lib_symbols.setdefault(sym.lib_id, sym)
        p = sym.pins[0]
        rot = 0
        for r in (0, 90, 180, 270):
            ox, oy = pin_outward(p, r)
            # power symbol body must extend away from the wire: body dir == -outward
            if (-ox, -oy) == tuple(direction):
                rot = r
                break
        self.project.pwr_count += 1
        ref = "#PWR%03d" % self.project.pwr_count
        inst = Instance(ref, sym, 1, at[0], at[1], rot, None,
                        {"Reference": ref, "Value": net, "Footprint": "", "Datasheet": "~",
                         "Description": "Power symbol"}, False, False, False)
        inst.uuid = uid("pwr", self.filename, ref)
        inst.nets["1"] = net
        inst.value_dir = direction
        self.instances.append(inst)
        self.project.register(inst, self, power=True)
        return inst

    def pwr_flag(self, net, at):
        sym = load_symbol("power:PWR_FLAG")
        self.lib_symbols.setdefault(sym.lib_id, sym)
        self.project.flag_count += 1
        ref = "#FLG%03d" % self.project.flag_count
        inst = Instance(ref, sym, 1, snap(at[0]), snap(at[1]), 0, None,
                        {"Reference": ref, "Value": "PWR_FLAG", "Footprint": "", "Datasheet": "~",
                         "Description": "Power flag"}, False, False, False)
        inst.uuid = uid("flg", self.filename, ref)
        inst.nets["1"] = net
        self.instances.append(inst)
        self.project.register(inst, self, power=True)
        # wire down 2.54 to a power symbol of the net
        x, y = inst.X, inst.Y
        self.wire((x, y), (x, y + 2.54))
        if net in POWER_NETS:
            self.power(net, (x, y + 2.54), (0, 1))
        else:
            self.glabel(net, (x, y + 2.54), (0, 1))
        self.project.nets.setdefault(net, [])
        return inst

    def text(self, s, at, size=1.27, bold=False):
        eff = ["effects", ["font", ["size", size, size]] + ([["bold", "yes"]] if bold else []),
               ["justify", "left", "top"]]
        self.items.append(["text", q(s), ["exclude_from_sim", "no"], ["at", at[0], at[1], 0], eff,
                           ["uuid", q(uid("txt", s, at))]])

    # ---- output --------------------------------------------------------
    def _symbol_node(self, inst, root_path):
        sym = inst.sym
        n = ["symbol", ["lib_id", q(sym.lib_id)], ["at", inst.X, inst.Y, inst.rot], ["unit", inst.unit]]
        if inst.mirror:
            n.append(["mirror", inst.mirror])
        n += [["exclude_from_sim", "no"], ["in_bom", "yes" if inst.in_bom else "no"],
              ["on_board", "yes" if inst.on_board else "no"], ["in_pos_files", "yes"],
              ["dnp", "yes" if inst.dnp else "no"], ["uuid", q(inst.uuid)]]
        # property placement: reference/value beside the body
        xs = [p.x for p in sym.unit_pins(inst.unit)] or [0]
        ys = [p.y for p in sym.unit_pins(inst.unit)] or [0]
        is_power = inst.ref.startswith("#")
        if is_power:
            # value text sits beyond the symbol body
            dx, dy = getattr(inst, "value_dir", (0, -1))
            vx, vy = inst.X + dx * 3.81, inst.Y + dy * 3.81
            props = [("Reference", inst.X, inst.Y, True, "left"),
                     ("Value", vx, vy, False, "left" if dx >= 0 else "right"),
                     ("Footprint", inst.X, inst.Y, True, "left"),
                     ("Datasheet", inst.X, inst.Y, True, "left"),
                     ("Description", inst.X, inst.Y, True, "left")]
        else:
            bx = max(xs) + 1.27 if inst.rot == 0 else max(ys) + 1.27
            by = -max(ys) + inst.Y if inst.rot == 0 else inst.Y
            rx, ry = inst.X + bx, min(by, inst.Y - 1.27)
            props = [("Reference", rx, ry, False, "left"),
                     ("Value", rx, ry + 2.54, False, "left"),
                     ("Footprint", rx, ry + 5.08, True, "left"),
                     ("Datasheet", rx, ry + 7.62, True, "left"),
                     ("Description", rx, ry + 10.16, True, "left")]
        done = set()
        for name, px, py, hide, just in props:
            val = inst.fields.get(name, "")
            node = ["property", q(name), q(val), ["at", round(px, 4), round(py, 4), 0]]
            if hide:
                node.append(["hide", "yes"])
            node.append(["effects", ["font", ["size", 1.27, 1.27]], ["justify", just]])
            n.append(node)
            done.add(name)
        k = 0
        for name, val in inst.fields.items():
            if name in done:
                continue
            k += 1
            n.append(["property", q(name), q(val), ["at", inst.X, inst.Y + 12.7 + 2.54 * k, 0],
                      ["hide", "yes"], ["effects", ["font", ["size", 1.27, 1.27]]]])
        for p in sym.unit_pins(inst.unit):
            n.append(["pin", q(p.number), ["uuid", q(uid("pin", inst.uuid, p.number))]])
        n.append(["instances", ["project", q(self.project.name),
                                ["path", q(root_path + "/" + self.symbol_uuid if root_path is not None else "/" + self.symbol_uuid),
                                 ["reference", q(inst.ref)], ["unit", inst.unit]]]])
        return n

    def write(self, path, root=None):
        root_uuid = self.project.root_uuid
        n = ["kicad_sch", ["version", 20260306], ["generator", q("eeschema")],
             ["generator_version", q("10.0")], ["uuid", q(self.uuid)], ["paper", q(self.paper)]]
        tb = ["title_block", ["title", q(self.title or self.name)], ["date", q(self.project.date)],
              ["rev", q(self.project.rev)], ["company", q(self.project.company)]]
        n.append(tb)
        libs = ["lib_symbols"]
        for lid in sorted(self.lib_symbols):
            libs.append(self.lib_symbols[lid].node)
        n.append(libs)
        n += self.items
        if root is None:
            path_prefix = "/" + root_uuid
            for inst in self.instances:
                n.append(self._symbol_node(inst, path_prefix))
        else:
            for inst in self.instances:
                n.append(self._symbol_node(inst, None))
            n.append(["sheet_instances", ["path", q("/"), ["page", q("1")]]])
        n.append(["embedded_fonts", "no"])
        with open(path, "w", encoding="utf-8") as f:
            f.write(dump(n))


class Project:
    def __init__(self, name, date, rev="A", company=""):
        self.name = name
        self.date = date
        self.rev = rev
        self.company = company
        self.root_uuid = uid("root", name)
        self.pwr_count = 0
        self.flag_count = 0
        self.sheets = []
        self.nets = {}
        self.components = {}  # ref -> (inst, sheet)
        self.custom_lib = None

    def register(self, inst, sheet, power=False):
        if not power:
            if inst.ref in self.components and self.components[inst.ref][0].sym.lib_id != inst.sym.lib_id:
                raise ValueError("duplicate reference " + inst.ref)
            self.components.setdefault(inst.ref, (inst, sheet))

    def add_sheet(self, name, filename, **kw):
        s = Sheet(self, name, filename, **kw)
        self.sheets.append(s)
        return s

    def write_root(self, path, filename):
        n = ["kicad_sch", ["version", 20260306], ["generator", q("eeschema")],
             ["generator_version", q("10.0")], ["uuid", q(self.root_uuid)], ["paper", q("A4")],
             ["title_block", ["title", q(self.name)], ["date", q(self.date)], ["rev", q(self.rev)],
              ["company", q(self.company)]],
             ["lib_symbols"]]
        x, y = 25.4, 25.4
        page = 2
        for s in self.sheets:
            sheet_uuid = s.symbol_uuid
            node = ["sheet", ["at", x, y], ["size", 40.64, 20.32], ["exclude_from_sim", "no"],
                    ["in_bom", "yes"], ["on_board", "yes"], ["dnp", "no"], ["fields_autoplaced", "yes"],
                    ["stroke", ["width", 0.1524], ["type", "solid"]], ["fill", ["color", 0, 0, 0, 0.0]],
                    ["uuid", q(sheet_uuid)],
                    ["property", q("Sheetname"), q(s.name), ["at", x, y - 0.7116, 0],
                     ["effects", ["font", ["size", 1.27, 1.27]], ["justify", "left", "bottom"]]],
                    ["property", q("Sheetfile"), q(s.filename), ["at", x, y + 20.32 + 0.5846, 0],
                     ["effects", ["font", ["size", 1.27, 1.27]], ["justify", "left", "top"]]],
                    ["instances", ["project", q(self.name),
                                   ["path", q("/" + self.root_uuid), ["page", q(str(page))]]]]]
            n.append(node)
            page += 1
            y += 30.48
            if y > 180:
                y = 25.4
                x += 60.96
        n.append(["sheet_instances", ["path", q("/"), ["page", q("1")]]])
        n.append(["embedded_fonts", "no"])
        with open(path, "w", encoding="utf-8") as f:
            f.write(dump(n))

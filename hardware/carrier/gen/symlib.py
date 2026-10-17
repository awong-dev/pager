"""Load KiCad symbols from .kicad_sym libraries and prepare them for embedding
in a schematic's lib_symbols block (flattened: no `extends`)."""

import copy
import os
import re

from sexp import Quoted, find, find_all, parse, q

KICAD_SYMBOLS = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"

_lib_cache = {}


def load_lib(path):
    if path not in _lib_cache:
        with open(path, encoding="utf-8") as f:
            nodes = parse(f.read())
        lib = nodes[0]
        syms = {}
        for s in find_all(lib, "symbol"):
            syms[str(s[1])] = s
        _lib_cache[path] = syms
    return _lib_cache[path]


class Pin:
    __slots__ = ("number", "name", "etype", "x", "y", "angle", "length", "hidden", "unit")

    def __init__(self, number, name, etype, x, y, angle, length, hidden, unit):
        self.number = number
        self.name = name
        self.etype = etype
        self.x = x
        self.y = y
        self.angle = angle
        self.length = length
        self.hidden = hidden
        self.unit = unit

    def __repr__(self):
        return "Pin(%s %s %s at %s,%s rot %s unit %s)" % (
            self.number, self.name, self.etype, self.x, self.y, self.angle, self.unit)


class Symbol:
    """A flattened symbol ready for embedding."""

    def __init__(self, lib_id, node):
        self.lib_id = lib_id  # e.g. "Device:R"
        self.node = node  # sexp node, renamed to lib_id
        self.pins = []
        self.units = 1
        self._scan()

    def _scan(self):
        base = str(self.node[1]).split(":", 1)[-1]
        units = set()
        for sub in find_all(self.node, "symbol"):
            m = re.match(r"^(.*)_(\d+)_(\d+)$", str(sub[1]))
            unit = int(m.group(2)) if m else 0
            if unit:
                units.add(unit)
            for p in find_all(sub, "pin"):
                etype = p[1]
                at = find(p, "at")
                ln = find(p, "length")
                hidden = find(p, "hide") is not None and find(p, "hide")[1] == "yes"
                name = find(p, "name")[1]
                number = find(p, "number")[1]
                self.pins.append(Pin(str(number), str(name), str(etype), float(at[1]), float(at[2]),
                                     float(at[3]) if len(at) > 3 else 0.0,
                                     float(ln[1]), hidden, unit))
        self.units = max(units) if units else 1
        self.base = base

    def unit_pins(self, unit):
        return [p for p in self.pins if p.unit in (0, unit)]

    def pin(self, number, unit=None):
        for p in self.pins:
            if p.number == str(number) and (unit is None or p.unit in (0, unit)):
                return p
        raise KeyError("%s has no pin %s" % (self.lib_id, number))

    def pin_by_name(self, name):
        for p in self.pins:
            if p.name == name:
                return p
        raise KeyError("%s has no pin named %s" % (self.lib_id, name))

    def prop(self, name):
        for p in find_all(self.node, "property"):
            if p[1] == name:
                return str(p[2])
        return None


def _flatten(syms, name):
    """Return a deep copy of symbol `name` with any `extends` resolved."""
    node = copy.deepcopy(syms[name])
    ext = find(node, "extends")
    if ext is None:
        return node
    parent = _flatten(syms, str(ext[1]))
    # child's properties override the parent's; parent's graphics/pins are kept
    child_props = {str(p[1]): p for p in find_all(node, "property")}
    merged = [parent[0], node[1]]
    for a in parent[2:]:
        if isinstance(a, list) and a and a[0] == "extends":
            continue
        if isinstance(a, list) and a and a[0] == "property":
            pname = str(a[1])
            if pname in child_props:
                merged.append(child_props.pop(pname))
                continue
        if isinstance(a, list) and a and a[0] == "symbol":
            # rename sub-symbols from parent_x_y to child_x_y
            a = copy.deepcopy(a)
            sub = str(a[1])
            pname = str(parent[1])
            if sub.startswith(pname + "_"):
                a[1] = q(name + sub[len(pname):])
            merged.append(a)
            continue
        merged.append(a)
    for p in child_props.values():
        merged.append(p)
    return merged


def load_symbol(lib_id, lib_path=None):
    """lib_id like "Device:R"; returns a Symbol whose node is renamed to the
    full lib_id as KiCad does when embedding."""
    lib, name = lib_id.split(":", 1)
    if lib_path is None:
        lib_path = os.path.join(KICAD_SYMBOLS, lib + ".kicad_sym")
    syms = load_lib(lib_path)
    if name not in syms:
        raise KeyError("%s not in %s" % (name, lib_path))
    node = _flatten(syms, name)
    node[1] = q(lib_id)
    # sub-symbol names keep the bare name (KiCad convention)
    return Symbol(lib_id, node)


if __name__ == "__main__":
    import sys

    for lid in sys.argv[1:]:
        s = load_symbol(lid)
        print(lid, "units", s.units, "fp", s.prop("Footprint"))
        for p in s.pins:
            print("   ", p)

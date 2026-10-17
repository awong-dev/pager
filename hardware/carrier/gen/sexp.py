"""Minimal S-expression reader/writer for KiCad files.

A node is a Python list whose first element is the token (str); atoms are
str (bare symbols), Quoted (quoted strings) or numbers kept as str so that
KiCad's formatting round-trips.
"""


class Quoted(str):
    """A quoted string atom."""

    __slots__ = ()


def parse(text):
    """Parse one or more top-level S-expressions; returns a list of nodes."""
    i = 0
    n = len(text)
    stack = [[]]
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "(":
            stack.append([])
            i += 1
        elif c == ")":
            node = stack.pop()
            stack[-1].append(node)
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n:
                d = text[j]
                if d == "\\" and j + 1 < n:
                    e = text[j + 1]
                    buf.append({"n": "\n", "t": "\t"}.get(e, e))
                    j += 2
                    continue
                if d == '"':
                    break
                buf.append(d)
                j += 1
            stack[-1].append(Quoted("".join(buf)))
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n()\"":
                j += 1
            stack[-1].append(text[i:j])
            i = j
    if len(stack) != 1:
        raise ValueError("unbalanced parentheses")
    return stack[0]


def q(s):
    return Quoted(str(s))


def num(v):
    """Format a number the way KiCad does (no trailing zeros)."""
    if isinstance(v, str):
        return v
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    if s in ("-0", ""):
        s = "0"
    return s


def dump(node, indent=0):
    """Serialise a node with KiCad-style tab indentation."""
    out = []
    _dump(node, indent, out)
    return "".join(out)


def _atom(a):
    if isinstance(a, Quoted):
        return '"' + a.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
    if isinstance(a, (int, float)):
        return num(a)
    return a


def _dump(node, indent, out):
    tab = "\t" * indent
    if not isinstance(node, list):
        out.append(tab + _atom(node))
        return
    head = []
    rest = []
    # leading atoms stay on the opening line, child lists go on their own lines
    seen_list = False
    for a in node:
        if isinstance(a, list):
            seen_list = True
            rest.append(a)
        elif not seen_list:
            head.append(_atom(a))
        else:
            rest.append(a)
    if not rest:
        out.append(tab + "(" + " ".join(head) + ")\n")
        return
    out.append(tab + "(" + " ".join(head) + "\n")
    for r in rest:
        if isinstance(r, list):
            _dump(r, indent + 1, out)
        else:
            out.append("\t" * (indent + 1) + _atom(r) + "\n")
    out.append(tab + ")\n")


def find(node, token):
    """First child list whose head is token."""
    for a in node:
        if isinstance(a, list) and a and a[0] == token:
            return a
    return None


def find_all(node, token):
    return [a for a in node if isinstance(a, list) and a and a[0] == token]


def get_prop(node, name):
    for p in find_all(node, "property"):
        if len(p) > 2 and p[1] == name:
            return p[2]
    return None


def set_prop(node, name, value):
    for p in find_all(node, "property"):
        if len(p) > 2 and p[1] == name:
            p[2] = q(value)
            return p
    return None

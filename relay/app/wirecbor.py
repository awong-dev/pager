"""CBOR encoding of the wire envelope, per docs/PROTOCOL.md §10 (normative,
added by task D0.1) and docs/DEVICE_PLAN.md §2.4.

Devices publish every envelope on `pager/{id}/...` as a **CBOR
definite-length map with integer keys**; JSON keeps the same *values* (same
strings, same enums, same numbers) under text keys for humans, logs and
`tools/send.py`. This module only renames keys and, for the four nested
object shapes the keymap defines, the keys one level down -- it never
changes a value's type or content (`docs/DEVICE_PLAN.md` §2.4: "values are
identical to the JSON ones").

`KEYMAP` is `PROTOCOL.md` §10's single flat integer namespace for envelope,
`/status`, bootstrap and `cfg` field *names* (0-38). Four more field names
nest one level deeper and get their own small local namespace, per §10's
"Sub-map keys" list: `loc` (the `/loc` fix object), `c[]` items (named
`contact` here), `p[]` items (named `request` here), and `cfg.lock`.

**Gap in `PROTOCOL.md` §10, flagged rather than silently resolved:** the
table gives `cfg`'s own numeric key (38) and `lock`'s *inner* keys
(`clear=0, auto=1`), but never assigns a numeric key to the `lock` field
*inside* the `cfg` map itself -- `cfg`'s JSON shape is
`{"lock": {"clear": ..., "auto": ...}}`, so that middle key needs one too.
`DEVICE_PLAN.md`'s pre-D0.1 wording ("`cfg`: `lock=38`") suggests `lock` was
briefly meant to *be* 38 before the table split `cfg`(38) out as the outer
field, and D0.1 dropped the inner slot when it did. Absent an explicit
number, this module assigns `lock` the same way every other sub-map assigns
its own first (and so far only) field -- `0` -- via `CFG_KEYMAP` below, kept
separate from `LOCK_KEYMAP` so a real allocation from `PROTOCOL.md` can
replace it without touching this module's structure.
"""

from __future__ import annotations

from typing import Any

import cbor2

# PROTOCOL.md §10: envelope 0-20, /status 21-28, bootstrap 29-37, cfg 38.
KEYMAP: dict[str, int] = {
    "v": 0,
    "id": 1,
    "ts": 2,
    "from": 3,
    "body": 4,
    "ack": 5,
    "kind": 6,
    "to": 7,
    "loc": 8,
    "req": 9,
    "cached": 10,
    "err": 11,
    "n": 12,
    "sig": 13,
    "bv": 14,
    "name": 15,
    "ph": 16,
    "d": 17,
    "c": 18,
    "p": 19,
    "more": 20,
    "state": 21,
    "mode": 22,
    "batt_mv": 23,
    "rssi": 24,
    "session": 25,
    "fw": 26,
    "loc_period_s": 27,
    "loc_min_s": 28,
    "ok": 29,
    "pw": 30,
    "k": 31,
    "host": 32,
    "port": 33,
    "ca": 34,
    "flags": 35,
    "label": 36,
    "apn": 37,
    "cfg": 38,
}
REVERSE_KEYMAP: dict[int, str] = {v: k for k, v in KEYMAP.items()}

# Sub-map keys, PROTOCOL.md §10 "Sub-map keys".
LOC_KEYMAP: dict[str, int] = {"lat": 0, "lon": 1, "acc": 2, "fix_ts": 3, "src": 4}
CONTACT_KEYMAP: dict[str, int] = {"a": 0, "n": 1, "t": 2}
REQUEST_KEYMAP: dict[str, int] = {"n": 0, "s": 1}
LOCK_KEYMAP: dict[str, int] = {"clear": 0, "auto": 1}
# See module docstring: not in PROTOCOL.md §10, this module's own filling of
# that gap.
CFG_KEYMAP: dict[str, int] = {"lock": 0}

_REVERSE_LOC = {v: k for k, v in LOC_KEYMAP.items()}
_REVERSE_CONTACT = {v: k for k, v in CONTACT_KEYMAP.items()}
_REVERSE_REQUEST = {v: k for k, v in REQUEST_KEYMAP.items()}
_REVERSE_LOCK = {v: k for k, v in LOCK_KEYMAP.items()}
_REVERSE_CFG = {v: k for k, v in CFG_KEYMAP.items()}


def _value_to_int_keys(name: str, value: Any) -> Any:
    if name == "loc" and isinstance(value, dict):
        return {LOC_KEYMAP[k]: v for k, v in value.items()}
    if name == "c" and isinstance(value, list):
        return [{CONTACT_KEYMAP[k]: v for k, v in item.items()} for item in value]
    if name == "p" and isinstance(value, list):
        return [{REQUEST_KEYMAP[k]: v for k, v in item.items()} for item in value]
    if name == "cfg" and isinstance(value, dict):
        out: dict[int, Any] = {}
        for k, v in value.items():
            if k == "lock" and isinstance(v, dict):
                out[CFG_KEYMAP["lock"]] = {LOCK_KEYMAP[lk]: lv for lk, lv in v.items()}
            else:
                # §3.2: "Unknown members of cfg are ignored" -- a future
                # member this module does not know a numeric key for yet
                # passes through under its JSON name rather than failing.
                out[k] = v
        return out
    return value


def _value_to_names(name: str, value: Any) -> Any:
    if name == "loc" and isinstance(value, dict):
        return {_REVERSE_LOC[k]: v for k, v in value.items()}
    if name == "c" and isinstance(value, list):
        return [{_REVERSE_CONTACT[k]: v for k, v in item.items()} for item in value]
    if name == "p" and isinstance(value, list):
        return [{_REVERSE_REQUEST[k]: v for k, v in item.items()} for item in value]
    if name == "cfg" and isinstance(value, dict):
        out: dict[str, Any] = {}
        for k, v in value.items():
            if k == CFG_KEYMAP["lock"] and isinstance(v, dict):
                out["lock"] = {_REVERSE_LOCK[lk]: lv for lk, lv in v.items()}
            else:
                out[_REVERSE_CFG.get(k, k)] = v
        return out
    return value


def translate_to_int(obj: dict[str, Any]) -> dict[int, Any]:
    """`obj` (JSON names) -> a dict keyed by `KEYMAP` integers, translating
    the four nested sub-maps one level down. Exposed (not just used inside
    `encode`) because `app/devauth.py`'s `sign_cbor` needs this int-keyed
    dict *before* `cbor2.dumps` to build the map header itself (§2.4's
    "header that includes the sig pair")."""
    return {KEYMAP[name]: _value_to_int_keys(name, value) for name, value in obj.items()}


def translate_to_names(raw: dict[int, Any]) -> dict[str, Any]:
    """Inverse of `translate_to_int`: an int-keyed dict (already CBOR-decoded)
    -> JSON names, translating the same four nested sub-maps back."""
    result: dict[str, Any] = {}
    for k, v in raw.items():
        name = REVERSE_KEYMAP[k]
        result[name] = _value_to_names(name, v)
    return result


def map_header(count: int) -> bytes:
    """CBOR definite-length map header (RFC 8949 §3.1, major type 5) for
    `count` pairs. `app/devauth.py` uses this directly to build a header
    whose count is one more (`sign_cbor`) or one less (`verify`) than the
    number of pairs actually present, per §2.4's signing rule."""
    if count < 24:
        return bytes([0xA0 | count])
    if count < 256:
        return bytes([0xB8, count])
    if count < 65536:
        return bytes([0xB9]) + count.to_bytes(2, "big")
    return bytes([0xBA]) + count.to_bytes(4, "big")


def parse_map_header(b: bytes) -> tuple[int, int]:
    """Reads the definite-length map header at the start of `b`. Returns
    `(count, header_len)`. Raises `ValueError` for anything this module does
    not itself produce (indefinite-length maps, or a first byte outside
    `is_cbor`'s 0xA0-0xBF range)."""
    if not b or not is_cbor(b):
        raise ValueError("not a CBOR definite-length map")
    first = b[0]
    info = first & 0x1F
    if info < 24:
        return info, 1
    if info == 24:
        return b[1], 2
    if info == 25:
        return int.from_bytes(b[1:3], "big"), 3
    if info == 26:
        return int.from_bytes(b[1:5], "big"), 5
    raise ValueError("unsupported CBOR map header (indefinite or reserved)")


def is_cbor(b: bytes) -> bool:
    """§3's first-byte dispatch rule: `0x7B` (JSON `{`) vs `0xA0-0xBF` (a
    CBOR definite-length map, major type 5, every additional-info form)."""
    return bool(b) and 0xA0 <= b[0] <= 0xBF


def encode(obj: dict[str, Any]) -> bytes:
    """`obj` (JSON names/values) -> a definite-length CBOR map with integer
    keys. Values are unchanged (`DEVICE_PLAN.md` §2.4) -- a `bytes` value
    (e.g. a `sig` a caller has already computed) is written as a CBOR byte
    string because that is what `cbor2` does with `bytes`, not because this
    function treats `sig` specially."""
    return cbor2.dumps(translate_to_int(obj))


def decode(b: bytes) -> dict[str, Any]:
    """Inverse of `encode`: CBOR bytes -> a dict with JSON names."""
    raw = cbor2.loads(b)
    if not isinstance(raw, dict):
        raise TypeError("top-level CBOR value is not a map")
    return translate_to_names(raw)

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
`/status`, bootstrap and `cfg` field *names* (0-43, 48 -- 44-47 are reserved
by `V02_DESIGN.md` §7 for a later device-SMS task and are not in this map).
Five field names nest one level deeper and get their own small local
namespace, per §10's "Sub-map keys" list: `loc` (the `/loc` fix object),
`c[]` items (named `contact` here), `p[]` items (named `request` here),
`cfg.lock`, and `cfg.ca` (v0.2, docs/V02_DESIGN.md §4.4/§7).

**Gap in `PROTOCOL.md` §10, closed by v0.2's own edit rather than left
open:** an earlier revision of this module flagged that the table gave
`cfg`'s own numeric key (38) and `lock`'s *inner* keys (`clear=0, auto=1`)
but never assigned a numeric key to the `lock` field *inside* the `cfg` map
itself. `PROTOCOL.md` §10 now states the `cfg` sub-map explicitly
(`lock=0, ca=1`, `sms=2` reserved) -- `CFG_KEYMAP` below is that allocation,
not this module's own guess any more.
"""

from __future__ import annotations

import base64
from typing import Any

import cbor2

# PROTOCOL.md §10: envelope 0-20, /status 21-28, bootstrap 29-37, cfg 38,
# v0.2 additions 39-43 + 48 (44-47 are docs/V02_DESIGN.md §6's sms_log keys --
# a later task's allocation, not touched here).
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
    # docs/V02_DESIGN.md §7 (CA trust + wider replay counter):
    "tls": 39,
    "ca_url": 40,
    "ca_sha": 41,
    "ca_fp": 42,
    "loc_backoff_s": 43,
    "sms_lost": 48,
}
REVERSE_KEYMAP: dict[int, str] = {v: k for k, v in KEYMAP.items()}

# Sub-map keys, PROTOCOL.md §10 "Sub-map keys".
LOC_KEYMAP: dict[str, int] = {"lat": 0, "lon": 1, "acc": 2, "fix_ts": 3, "src": 4}
CONTACT_KEYMAP: dict[str, int] = {"a": 0, "n": 1, "t": 2}
REQUEST_KEYMAP: dict[str, int] = {"n": 0, "s": 1}
LOCK_KEYMAP: dict[str, int] = {"clear": 0, "auto": 1}
# docs/V02_DESIGN.md §7: `cfg.ca = {url, sha}`.
CA_KEYMAP: dict[str, int] = {"url": 0, "sha": 1}
# PROTOCOL.md §10's own `cfg` sub-map allocation (see module docstring):
# `lock=0`, `ca=1`; `sms=2` is reserved there for a later task
# (docs/V02_DESIGN.md §6) and deliberately not added here.
CFG_KEYMAP: dict[str, int] = {"lock": 0, "ca": 1}

_REVERSE_LOC = {v: k for k, v in LOC_KEYMAP.items()}
_REVERSE_CONTACT = {v: k for k, v in CONTACT_KEYMAP.items()}
_REVERSE_REQUEST = {v: k for k, v in REQUEST_KEYMAP.items()}
_REVERSE_LOCK = {v: k for k, v in LOCK_KEYMAP.items()}
_REVERSE_CA = {v: k for k, v in CA_KEYMAP.items()}
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
            elif k == "ca" and isinstance(v, dict):
                # docs/V02_DESIGN.md §7: `cfg.ca = {url, sha}`; `sha` is
                # absent on an un-pin push (`url=""`, §4.4).
                out[CFG_KEYMAP["ca"]] = {CA_KEYMAP[ck]: cv for ck, cv in v.items()}
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
            elif k == CFG_KEYMAP["ca"] and isinstance(v, dict):
                out["ca"] = {_REVERSE_CA[ck]: cv for ck, cv in v.items()}
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
    -> JSON names, translating the same four nested sub-maps back.

    docs/PROTOCOL.md §0/§3.1's forward-compatibility rule ("unknown fields
    MUST be ignored, not rejected") applies to a CBOR integer key this
    module's `KEYMAP` does not know yet, exactly as it applies to an unknown
    JSON name -- a newer device may already send one (docs/V02_DESIGN.md §0:
    "add the relay's acceptance of new fields before any firmware that sends
    them is flashed"). A key outside `REVERSE_KEYMAP` is therefore dropped,
    not raised: this used to `KeyError`, which propagated out of `decode()`
    and made `app/wire.py`'s `decode_envelope_bytes` treat the *entire*
    envelope as malformed -- silently rejecting, say, a whole `/status` just
    because it carried one field key this relay predates."""
    result: dict[str, Any] = {}
    for k, v in raw.items():
        name = REVERSE_KEYMAP.get(k)
        if name is None:
            continue
        result[name] = _value_to_names(name, v)
    return result


def to_json_safe(obj: Any) -> Any:
    """Recursively replaces every `bytes` value with base64url text, no
    padding -- docs/V02_DESIGN.md §7: "`ca_sha`/`sha` are base64url without
    padding, like `sig`." A `/down cfg.ca` envelope's `sha` (and any future
    bstr-typed sub-field) is built as raw `bytes` regardless of which wire
    encoding it ends up published in (the same value `encode()`'s CBOR path
    writes as a bstr unchanged); `json.dumps` cannot serialise `bytes` at
    all, so this is the JSON path's equivalent conversion, applied by
    `app/devauth.py`'s `sign_json` and `app/broker.py`'s unsigned-JSON
    publish path just before serialising."""
    if isinstance(obj, bytes):
        return base64.urlsafe_b64encode(obj).rstrip(b"=").decode("ascii")
    if isinstance(obj, dict):
        return {k: to_json_safe(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [to_json_safe(v) for v in obj]
    return obj


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

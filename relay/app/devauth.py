"""Device-authenticated envelope signing and verification, per
`docs/PROTOCOL.md` §14 (normative, added by D0.1) and `docs/DEVICE_PLAN.md`
§2.4.

Every `/up`, `/status` and `/loc` from an `authMode: "hmac"` device carries
`n` (replay counter, `app/store/device_secrets.py`) and `sig` (an
HMAC-SHA256 tag, truncated to 64 bits). This module computes and checks
`sig` only -- it never touches `n` or the replay window (that is
`app/store/device_secrets.py`'s `accept_up_n`), and it never parses the rest
of the envelope: `verify` reads only the trailing signature bytes before
deciding whether the payload may be parsed at all (§3.4, §14.4: "Parsing
happens only after verification succeeds").
"""

from __future__ import annotations

import base64
import hmac
import json
import re
from typing import Any

import cbor2

from app import wirecbor

# §14.3 / DEVICE_PLAN.md §2.4: `sig` is CBOR key 13, an 8-byte string --
# `0x0D` (small uint 13) then `0x48` (bstr, length 8).
_SIG_KEY = wirecbor.KEYMAP["sig"]
_CBOR_SIG_MARKER = bytes([_SIG_KEY, 0x40 | 8])
_CBOR_SIG_LEN = len(_CBOR_SIG_MARKER) + 8  # 10 bytes total, per §14.3.

# §14.3 / DEVICE_PLAN.md §2.4: JSON emits `,"sig":"<11 base64url chars>"}` as
# the last thing in the payload -- 8 raw bytes, base64url, no padding, is
# always exactly 11 characters (ceil(8 * 4 / 3) with no trailing `=`).
_JSON_SIG_RE = re.compile(rb',"sig":"([A-Za-z0-9_-]{11})"\}\Z')


def tag(key: bytes, topic: str, p: bytes) -> bytes:
    """§14.3: `HMAC-SHA256(K_dev, topic \\x00 P)[0:8]`."""
    return hmac.new(key, topic.encode("utf-8") + b"\x00" + p, "sha256").digest()[:8]


# §14.7: the book-pull HTTPS request's MAC uses this literal string "where
# §14.3 puts the topic" -- no MQTT topic contains a space, so a request tag
# can never verify as an envelope tag or the reverse (domain separation).
_BOOK_REQUEST_TOPIC = "GET /api/device/book"


def request_tag(key: bytes, device_id: str, n: int, bv: int) -> bytes:
    """§14.7: `X-Sig = base64url(HMAC-SHA256(K_dev, "GET /api/device/book"
    \\x00 M)[0:8])` with `M = ASCII "<device_id>|<n>|<bv>"`, `n` and `bv`
    exactly as sent in `X-N` and the query. Returns the raw 8-byte tag (not
    base64url-encoded) -- callers that need the wire form encode it
    themselves, same split as `tag()` above."""
    m = f"{device_id}|{n}|{bv}".encode("ascii")
    return tag(key, _BOOK_REQUEST_TOPIC, m)


def sign_cbor(key: bytes, topic: str, obj: dict[str, Any]) -> bytes:
    """§14.3's CBOR rule: encode `obj` (which must not already contain
    `sig`) as a map, but with a header count one higher than the number of
    pairs written -- this is `P`, everything the tag covers -- then append
    the fixed 10-byte `sig` pair (`0x0D 0x48` + the 8-byte tag) so the
    header's count and the pairs actually present agree once more."""
    if "sig" in obj:
        raise ValueError("obj must not already contain sig")
    fields = wirecbor.translate_to_int(obj)
    body = cbor2.dumps(fields)
    count, header_len = wirecbor.parse_map_header(body)
    p = wirecbor.map_header(count + 1) + body[header_len:]
    return p + _CBOR_SIG_MARKER + tag(key, topic, p)


def sign_json(key: bytes, topic: str, obj: dict[str, Any]) -> bytes:
    """§14.3's JSON rule: minified JSON without `sig` (that is `P`, ending in
    `}`), MAC it, then emit `P[:-1] + ',"sig":"<base64url tag>"}'`.

    `wirecbor.to_json_safe` first turns any raw `bytes` leaf (e.g. a
    `/down cfg.ca.sha`, docs/V02_DESIGN.md §7 -- built as `bytes` regardless
    of which wire encoding it is published in) into base64url text, since
    `json.dumps` cannot serialise `bytes` at all; every other value passes
    through unchanged."""
    if "sig" in obj:
        raise ValueError("obj must not already contain sig")
    p = json.dumps(wirecbor.to_json_safe(obj), separators=(",", ":"), ensure_ascii=False).encode(
        "utf-8"
    )
    if not p.endswith(b"}"):
        raise ValueError("obj did not serialise to a JSON object")
    b64 = base64.urlsafe_b64encode(tag(key, topic, p)).rstrip(b"=").decode("ascii")
    return p[:-1] + f',"sig":"{b64}"}}'.encode()


def verify(key: bytes, topic: str, payload: bytes) -> tuple[bool, bytes]:
    """§14.3/§14.4: detect the encoding from the first byte, require the
    exact trailing `sig` shape for that encoding, verify in constant time,
    and return `(ok, payload_without_sig)` -- never decoding/parsing `obj`
    itself. On failure returns `(False, b"")`; the caller (a later task,
    `app/ingest.py`) treats that identically to a malformed payload (§3.4)."""
    if wirecbor.is_cbor(payload):
        return _verify_cbor(key, topic, payload)
    return _verify_json(key, topic, payload)


def _verify_cbor(key: bytes, topic: str, payload: bytes) -> tuple[bool, bytes]:
    if len(payload) < _CBOR_SIG_LEN:
        return False, b""
    marker = payload[-_CBOR_SIG_LEN : -_CBOR_SIG_LEN + 2]
    if marker != _CBOR_SIG_MARKER:
        return False, b""
    got_tag = payload[-8:]
    p = payload[: -_CBOR_SIG_LEN]
    if not hmac.compare_digest(got_tag, tag(key, topic, p)):
        return False, b""
    # p is header(count+1) + `count` real pairs; rewrite the header down to
    # `count` so the result is valid, directly decodable CBOR with the sig
    # pair simply absent (mirrors the JSON branch re-closing with `}`).
    count_plus_one, header_len = wirecbor.parse_map_header(p)
    without_sig = wirecbor.map_header(count_plus_one - 1) + p[header_len:]
    return True, without_sig


def _verify_json(key: bytes, topic: str, payload: bytes) -> tuple[bool, bytes]:
    match = _JSON_SIG_RE.search(payload)
    if match is None:
        return False, b""
    b64 = match.group(1).decode("ascii")
    try:
        got_tag = base64.urlsafe_b64decode(b64 + "=" * (-len(b64) % 4))
    except ValueError:
        return False, b""
    if len(got_tag) != 8:
        return False, b""
    p = payload[: match.start()] + b"}"
    if not hmac.compare_digest(got_tag, tag(key, topic, p)):
        return False, b""
    return True, p

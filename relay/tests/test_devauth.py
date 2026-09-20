"""Unit tests for `app.devauth` and `app.wirecbor`, per `docs/PROTOCOL.md`
§14 and §10 and `docs/DEVICE_PLAN.md` §2.4.

Vectors come from `tools/authvectors.json`, generated with this same code
(there is no independent implementation to check against yet -- the
byte-by-byte hand check in `test_first_vector_cbor_bytes_by_hand` is what
pins the format itself, not just this module's self-consistency)."""

from __future__ import annotations

import base64
import json
from pathlib import Path
from typing import Any

import cbor2
import pytest

from app import devauth, wire, wirecbor

VECTORS_PATH = Path(__file__).resolve().parents[2] / "tools" / "authvectors.json"
VECTORS: list[dict[str, Any]] = json.loads(VECTORS_PATH.read_text())


def _key(vector: dict[str, Any]) -> bytes:
    return base64.b64decode(vector["key_b64"])


def test_at_least_eight_vectors_covering_the_required_kinds():
    assert len(VECTORS) >= 8
    labels = {v["label"] for v in VECTORS}
    assert labels == {
        "down_msg",
        "ack",
        "up_msg_to",
        "status",
        "loc",
        "book",
        "cfg",
        "contact_req",
    }


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["label"])
def test_json_vector_round_trips(vector: dict[str, Any]):
    key = _key(vector)
    topic = vector["topic"]
    obj = vector["obj"]

    signed = devauth.sign_json(key, topic, obj)
    assert signed == base64.b64decode(vector["json_signed_b64"])

    ok, without_sig = devauth.verify(key, topic, signed)
    assert ok
    assert json.loads(without_sig) == obj


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["label"])
def test_cbor_vector_round_trips(vector: dict[str, Any]):
    key = _key(vector)
    topic = vector["topic"]
    obj = vector["obj"]

    signed = devauth.sign_cbor(key, topic, obj)
    assert signed == base64.b64decode(vector["cbor_signed_b64"])

    ok, without_sig = devauth.verify(key, topic, signed)
    assert ok
    assert wirecbor.decode(without_sig) == obj


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["label"])
def test_json_flipping_any_byte_fails(vector: dict[str, Any]):
    key = _key(vector)
    signed = base64.b64decode(vector["json_signed_b64"])
    for i in range(len(signed)):
        corrupted = bytearray(signed)
        corrupted[i] ^= 0x01
        ok, _ = devauth.verify(key, vector["topic"], bytes(corrupted))
        assert not ok, f"byte {i} flip in JSON vector {vector['label']!r} was not detected"


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["label"])
def test_cbor_flipping_any_byte_fails(vector: dict[str, Any]):
    key = _key(vector)
    signed = base64.b64decode(vector["cbor_signed_b64"])
    for i in range(len(signed)):
        corrupted = bytearray(signed)
        corrupted[i] ^= 0x01
        ok, _ = devauth.verify(key, vector["topic"], bytes(corrupted))
        assert not ok, f"byte {i} flip in CBOR vector {vector['label']!r} was not detected"


def test_json_wrong_key_fails():
    vector = VECTORS[0]
    signed = base64.b64decode(vector["json_signed_b64"])
    wrong_key = bytes((b + 1) % 256 for b in base64.b64decode(vector["key_b64"]))
    ok, _ = devauth.verify(wrong_key, vector["topic"], signed)
    assert not ok


def test_cbor_wrong_key_fails():
    vector = VECTORS[0]
    signed = base64.b64decode(vector["cbor_signed_b64"])
    wrong_key = bytes((b + 1) % 256 for b in base64.b64decode(vector["key_b64"]))
    ok, _ = devauth.verify(wrong_key, vector["topic"], signed)
    assert not ok


def test_json_sig_not_last_fails():
    """§14.3: `sig` MUST be the last pair -- a payload that carries a
    perfectly valid tag anywhere else is still malformed."""
    vector = next(v for v in VECTORS if v["label"] == "down_msg")
    key = _key(vector)
    signed = base64.b64decode(vector["json_signed_b64"])
    # Move `,"sig":"...".` from just before the final `}` to just after `{`.
    body = signed.decode("utf-8")
    assert body.endswith("}")
    head, sig_part = body[1:-1].rsplit(",", 1)
    assert sig_part.startswith('"sig":')
    reordered = "{" + sig_part + "," + head + "}"
    ok, _ = devauth.verify(key, vector["topic"], reordered.encode("utf-8"))
    assert not ok


def test_cbor_sig_not_last_fails():
    """§14.3: the CBOR verifier requires the payload to *end* in
    `0x0D 0x48 <8 bytes>` -- a map with the same pairs in a different order
    (sig pair moved off the end) must fail."""
    vector = next(v for v in VECTORS if v["label"] == "down_msg")
    key = _key(vector)
    signed = base64.b64decode(vector["cbor_signed_b64"])
    header = signed[:1]
    pairs = signed[1:-10]
    sig_pair = signed[-10:]
    reordered = header + sig_pair + pairs
    ok, _ = devauth.verify(key, vector["topic"], reordered)
    assert not ok


def test_truncated_json_missing_sig_fails():
    ok, _ = devauth.verify(b"k" * 32, "pager/pgr-0001/up", b'{"v":1,"id":"m_aaaaaaaa"}')
    assert not ok


def test_truncated_cbor_missing_sig_fails():
    ok, _ = devauth.verify(b"k" * 32, "pager/pgr-0001/up", wirecbor.encode({"v": 1}))
    assert not ok


def test_sign_json_rejects_obj_that_already_has_sig():
    with pytest.raises(ValueError):
        devauth.sign_json(b"k" * 32, "pager/pgr-0001/up", {"v": 1, "sig": "x"})


def test_sign_cbor_rejects_obj_that_already_has_sig():
    with pytest.raises(ValueError):
        devauth.sign_cbor(b"k" * 32, "pager/pgr-0001/up", {"v": 1, "sig": b"x"})


def test_first_vector_cbor_bytes_by_hand():
    """Hand-decoded byte-by-byte, per DEVICE_TASKS.md S1.2's requirement to
    check one CBOR vector against the spec independent of this module's own
    encoder. Vector: `down_msg`, obj
    `{"v":1,"id":"m_7f3a2b10","ts":1757700000,"from":"parent",
    "body":"be there at 5","ack":null}` on topic `pager/pgr-0001/down`.

    Full signed CBOR (56 bytes), hex:
    a7 00 01 01 6a 6d5f3766336132623130 02 1a68c45fa0 03 66 706172656e74
    04 6d 6265207468657265206174203 5 05 f6 0d 48 8244d598bacf0895

    - `a7`        map header, major type 5, count 7 (6 real fields + sig)
    - `00 01`     key 0 (`v`) = uint 1
    - `01 6a...`  key 1 (`id`) = tstr(10) "m_7f3a2b10"
    - `02 1a68c45fa0` key 2 (`ts`) = uint32 0x68c45fa0 = 1757700000
    - `03 66...`  key 3 (`from`) = tstr(6) "parent"
    - `04 6d...`  key 4 (`body`) = tstr(13) "be there at 5"
    - `05 f6`     key 5 (`ack`) = null
    - `0d 48 8244d598bacf0895` key 13 (`sig`) = bstr(8), the HMAC tag

    The tag covers everything up to (not including) the `0d 48` marker --
    `topic + b"\\x00" +` the first 46 bytes above -- with
    `HMAC-SHA256(key, ...)  [0:8] == 8244d598bacf0895` for
    `key = bytes(range(32))`, `topic = "pager/pgr-0001/down"`.
    """
    vector = next(v for v in VECTORS if v["label"] == "down_msg")
    signed = base64.b64decode(vector["cbor_signed_b64"])
    expected_hex = (
        "a70001016a6d5f3766336132623130021a68c45fa0"
        "0366706172656e74046d6265207468657265206174203505f60d488244d598bacf0895"
    )
    assert signed.hex() == expected_hex
    assert signed[0] == 0xA7  # map header, count 7
    assert signed[1:3] == bytes([0x00, 0x01])  # v: 1
    assert signed[-10:] == bytes.fromhex("0d488244d598bacf0895")

    import hmac as hmac_mod

    prefix = signed[:-10]
    key = _key(vector)
    expected_tag = hmac_mod.new(
        key, vector["topic"].encode("utf-8") + b"\x00" + prefix, "sha256"
    ).digest()[:8]
    assert signed[-8:] == expected_tag


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["label"])
def test_wirecbor_decode_encode_round_trips_every_vector_obj(vector: dict[str, Any]):
    obj = vector["obj"]
    assert wirecbor.decode(wirecbor.encode(obj)) == obj


def test_is_cbor_detects_first_byte():
    assert wirecbor.is_cbor(bytes([0xA0]))
    assert wirecbor.is_cbor(bytes([0xBF]))
    assert not wirecbor.is_cbor(b"{}")
    assert not wirecbor.is_cbor(b"")


# ---------------------------------------------------------------------------
# docs/PROTOCOL.md §0/§3.1's forward-compatibility rule, applied to CBOR
# integer keys: an unknown key must be dropped, not make the whole envelope
# fail to decode (docs/V02_DESIGN.md ground rule 0 -- "unknown ... /status
# fields from a newer device must not make an older relay reject the
# envelope"). Before this fix, `wirecbor.translate_to_names` raised
# `KeyError` on an unknown key, which `app/wire.py`'s `decode_envelope_bytes`
# caught and turned into "malformed, drop the whole envelope".
# ---------------------------------------------------------------------------

def test_decode_drops_a_single_unknown_integer_key_not_the_whole_envelope():
    # Key 99 is not in wirecbor.KEYMAP (a newer relay-only allocation, or a
    # future key this relay predates) alongside two keys this relay knows.
    raw = cbor2.dumps({0: 1, 21: "online", 99: "from-the-future"})
    decoded = wirecbor.decode(raw)
    assert decoded == {"v": 1, "state": "online"}


def test_decode_envelope_bytes_accepts_cbor_with_an_unknown_key():
    raw = cbor2.dumps({0: 1, 21: "online", 25: "s_aabbccdd", 99: "from-the-future"})
    result = wire.decode_envelope_bytes(raw)
    assert result is not None
    data, encoding = result
    assert encoding == "cbor"
    assert data == {"v": 1, "state": "online", "session": "s_aabbccdd"}


def test_to_json_safe_base64url_encodes_bytes_recursively():
    obj = {"cfg": {"ca": {"url": "https://x/y.pem", "sha": b"\x00\x01\xfe\xff"}}}
    safe = wirecbor.to_json_safe(obj)
    assert safe["cfg"]["ca"]["url"] == "https://x/y.pem"
    assert safe["cfg"]["ca"]["sha"] == "AAH-_w"  # base64url, no padding
    # Round-trips through json.dumps without error (the whole point).
    json.dumps(safe)


def test_to_json_safe_leaves_non_bytes_values_unchanged():
    obj = {"a": 1, "b": [1, "x", None, {"c": True}]}
    assert wirecbor.to_json_safe(obj) == obj

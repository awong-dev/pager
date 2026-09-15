"""Unit tests for `app.devsetup` and `app.store.setup_codes` --
docs/DEVICE_TASKS.md S2b.1, docs/DEVICE_PLAN.md §3.1, §3.2.

The cross-checkable vector lives in `tools/setup_code_vectors.json`, not
`tools/authvectors.json` -- see that file's own `_comment` for why (appending
a differently-shaped vector to the devauth vectors file would break
`test_devauth.py`, outside this task's `Files` list, without ever touching
it)."""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import cbor2
import pytest
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from app import devsetup, wirecbor
from app.config import Settings
from app.store import setup_codes as setup_codes_store
from tests.fake_transport import FakeBrokerClient

VECTORS_PATH = Path(__file__).resolve().parents[2] / "tools" / "setup_code_vectors.json"
VECTORS: list[dict[str, Any]] = json.loads(VECTORS_PATH.read_text())
VECTOR = VECTORS[0]


def _settings() -> Settings:
    return Settings(
        broker_api_url="http://emqx.test/api/v5",
        broker_api_key="k",
        broker_api_secret="s",
        webhook_key="wk",
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
    )


@dataclass
class _FakeEmqx:
    """Duck-typed stand-in for `app.emqx_admin.EmqxAdmin` -- `devsetup` only
    ever calls `ensure_boot_user`/`delete_user` on whatever it's given, so a
    plain recorder is enough (same spirit as `FakeBrokerClient`)."""

    ensured: list[tuple[str, str]] = field(default_factory=list)
    deleted: list[str] = field(default_factory=list)

    def ensure_boot_user(self, bid: str, password: str) -> str:
        self.ensured.append((bid, password))
        return "ok"

    def delete_user(self, username: str) -> str:
        self.deleted.append(username)
        return "ok"


# ---------------------------------------------------------------------------
# token() / format_code() / parse()
# ---------------------------------------------------------------------------


def test_token_round_trips_through_format_and_parse():
    raw, token_str = devsetup.token()
    assert len(token_str) == devsetup.TOKEN_CHARS + 1
    code = devsetup.format_code(token_str, host="broker.example.com")
    parsed_raw, host, port, apn = devsetup.parse(code)
    assert parsed_raw == raw
    assert host == "broker.example.com"
    assert port == devsetup.DEFAULT_PORT
    assert apn is None


def test_parse_accepts_spaces_hyphens_mixed_case_port_and_apn():
    raw, token_str = devsetup.token()
    code = devsetup.format_code(token_str, host="a.b-c.example.com", port=1883, apn="fast.t-mobile.com")
    # Mangle the token portion: lowercase it, swap hyphens for spaces, add
    # extra whitespace -- all of which §3.1 says entry must tolerate.
    token_part, rest = code.split("@", 1)
    mangled_token = token_part.strip().lower().replace("-", " ")
    mangled_code = f"  {mangled_token}   @{rest}"
    parsed_raw, host, port, apn = devsetup.parse(mangled_code)
    assert parsed_raw == raw
    assert host == "a.b-c.example.com"
    assert port == 1883
    assert apn == "fast.t-mobile.com"


def test_parse_rejects_a_one_character_typo_in_the_token():
    _, token_str = devsetup.token()
    code = devsetup.format_code(token_str, host="broker.example.com")
    token_part, rest = code.split("@", 1)
    bad_char = "0" if token_part[0] != "0" else "1"
    mangled = bad_char + token_part[1:]
    with pytest.raises(ValueError, match="check character"):
        devsetup.parse(f"{mangled}@{rest}")


def test_parse_rejects_a_transposition_typo_in_the_token():
    _, token_str = devsetup.token()
    code = devsetup.format_code(token_str, host="broker.example.com")
    token_part, rest = code.split("@", 1)
    chars = list(token_part.replace("-", ""))
    if chars[0] == chars[1]:
        pytest.skip("degenerate token for this property, regenerate")
    chars[0], chars[1] = chars[1], chars[0]
    with pytest.raises(ValueError, match="check character"):
        devsetup.parse(f"{''.join(chars)}@{rest}")


def test_parse_rejects_missing_at_sign():
    with pytest.raises(ValueError, match="@"):
        devsetup.parse("0000-0000-0000-00")


def test_parse_rejects_invalid_character():
    with pytest.raises(ValueError):
        devsetup.parse("UUUU-UUUU-UUUU-UU @ broker.example.com")


def test_format_code_omits_port_suffix_only_when_default():
    _, token_str = devsetup.token()
    assert " @ broker.example.com" in devsetup.format_code(token_str, host="broker.example.com")
    assert " @ broker.example.com:1883" in devsetup.format_code(
        token_str, host="broker.example.com", port=1883
    )


# ---------------------------------------------------------------------------
# derive() against the vector
# ---------------------------------------------------------------------------


def test_derive_matches_the_vector():
    token_bytes = base64.b64decode(VECTOR["token_b64"])
    bid, bpw, bkey = devsetup.derive(token_bytes)
    assert bid == VECTOR["bid"]
    assert bpw == VECTOR["bpw"]
    assert bkey == base64.b64decode(VECTOR["bkey_b64"])


def test_encode_token_matches_the_vectors_code():
    token_bytes = base64.b64decode(VECTOR["token_b64"])
    token_str = devsetup._encode_token(token_bytes)
    expected_token_part = VECTOR["code"].split("@", 1)[0].strip()
    assert expected_token_part.replace("-", "") == token_str


# ---------------------------------------------------------------------------
# bundle() / decrypt_bundle() -- including the vector's fixed ciphertext.
# ---------------------------------------------------------------------------


def _vector_device() -> devsetup.BootstrapDevice:
    obj = VECTOR["bundle_plain_obj"]
    return devsetup.BootstrapDevice(
        id=obj["id"],
        pw=obj["pw"],
        k=base64.b64decode(obj["k_b64"]),
        host=obj["host"],
        port=obj["port"],
        ca=obj["ca"],
        flags=obj["flags"],
        label=obj["label"],
    )


def test_decrypting_the_vector_bundle_yields_the_plaintext_object():
    bkey = base64.b64decode(VECTOR["bkey_b64"])
    blob = base64.b64decode(VECTOR["bundle_ct_b64"])
    decoded = devsetup.decrypt_bundle(bkey, blob)
    expected = dict(VECTOR["bundle_plain_obj"])
    k_b64 = expected.pop("k_b64")
    expected["k"] = base64.b64decode(k_b64)
    assert decoded == expected


def test_vector_bundle_decrypts_by_hand_with_independent_primitives():
    """Independent of `devsetup.decrypt_bundle`/`wirecbor`: decrypt with
    `cryptography`'s AESGCM directly and decode with bare `cbor2`, then check
    the raw integer keys against `docs/PROTOCOL.md` §10's boot keymap
    (0, 1, 30-36) by hand -- pins the wire format itself, not just this
    module's internal self-consistency (same reasoning as
    `test_devauth.test_first_vector_cbor_bytes_by_hand`)."""
    bkey = base64.b64decode(VECTOR["bkey_b64"])
    blob = base64.b64decode(VECTOR["bundle_ct_b64"])
    nonce, ct = blob[:12], blob[12:]
    assert nonce == base64.b64decode(VECTOR["bundle_nonce_b64"])
    plaintext = AESGCM(bkey).decrypt(nonce, ct, None)
    raw = cbor2.loads(plaintext)
    assert raw[0] == 1  # v
    assert raw[1] == VECTOR["bundle_plain_obj"]["id"]
    assert raw[30] == VECTOR["bundle_plain_obj"]["pw"]
    assert raw[31] == base64.b64decode(VECTOR["bundle_plain_obj"]["k_b64"])
    assert raw[32] == VECTOR["bundle_plain_obj"]["host"]
    assert raw[33] == VECTOR["bundle_plain_obj"]["port"]
    assert raw[34] == VECTOR["bundle_plain_obj"]["ca"]
    assert raw[35] == VECTOR["bundle_plain_obj"]["flags"]
    assert raw[36] == VECTOR["bundle_plain_obj"]["label"]


def test_bundle_round_trips_with_a_fresh_random_nonce():
    device = devsetup.BootstrapDevice(
        id="pgr-0002",
        pw="pw",
        k=b"k" * 32,
        host="broker.example.com",
        ca="-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n",
        flags=0,
        label="Kid 2",
        apn="internet",
    )
    bkey = b"b" * 32
    blob1 = devsetup.bundle(device, bkey)
    blob2 = devsetup.bundle(device, bkey)
    assert blob1 != blob2  # random nonce each call
    for blob in (blob1, blob2):
        decoded = devsetup.decrypt_bundle(bkey, blob)
        assert decoded == {
            "v": 1,
            "id": "pgr-0002",
            "pw": "pw",
            "k": b"k" * 32,
            "host": "broker.example.com",
            "port": devsetup.DEFAULT_PORT,
            "ca": device.ca,
            "flags": 0,
            "label": "Kid 2",
            "apn": "internet",
        }


def test_decrypt_bundle_wrong_key_raises_invalid_tag():
    device = _vector_device()
    bkey = base64.b64decode(VECTOR["bkey_b64"])
    blob = devsetup.bundle(device, bkey)
    with pytest.raises(InvalidTag):
        devsetup.decrypt_bundle(b"x" * 32, blob)


def test_bundle_uses_wirecbor_keymap_for_every_field():
    """Every JSON name `bundle()` writes has the exact numeric key
    `docs/PROTOCOL.md` §10 assigns it -- `app/wirecbor.KEYMAP` is the single
    source of truth for that mapping, already exercised independently by
    `app/wirecbor.py`'s own tests; this just pins that `bundle()` uses it for
    every one of its own field names."""
    for name in ("v", "id", "pw", "k", "host", "port", "ca", "flags", "label", "apn"):
        assert name in wirecbor.KEYMAP


# ---------------------------------------------------------------------------
# issue() / complete() / expire()
# ---------------------------------------------------------------------------


def _issue(device_id: str, broker: FakeBrokerClient, emqx: _FakeEmqx, **kwargs: Any) -> str:
    return devsetup.issue(
        device_id,
        mqtt_password=kwargs.pop("mqtt_password", "real-mqtt-password"),
        hmac_key=kwargs.pop("hmac_key", b"h" * 32),
        host=kwargs.pop("host", "broker.example.com"),
        ca=kwargs.pop("ca", "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"),
        label=kwargs.pop("label", "Kid 1"),
        settings=_settings(),
        broker=broker,  # type: ignore[arg-type]
        emqx=emqx,  # type: ignore[arg-type]
        **kwargs,
    )


def test_issue_pushes_boot_user_publishes_retained_bundle_and_writes_setup_code():
    broker = FakeBrokerClient()
    emqx = _FakeEmqx()
    code = _issue("pgr-issue-1", broker, emqx)

    assert " @ broker.example.com" in code
    assert len(emqx.ensured) == 1
    bid, _bpw = emqx.ensured[0]
    assert emqx.deleted == []

    assert len(broker.published) == 1
    msg = broker.published[0]
    assert msg.topic == f"pager/boot/{bid}/down"
    assert msg.qos == 1
    assert msg.retain is True
    assert len(msg.payload) > 0

    stored = setup_codes_store.get(bid)
    assert stored is not None
    assert stored.deviceId == "pgr-issue-1"
    # Never store the token: `SetupCode` models only `bid`/`deviceId`/
    # `expiresAt` (app/store/setup_codes.py) -- there is no field the token,
    # `bpw` or `bkey` could even be written to.
    assert set(type(stored).model_fields) == {"bid", "deviceId", "expiresAt"}


def test_issue_bundle_decrypts_to_the_supplied_secrets():
    broker = FakeBrokerClient()
    emqx = _FakeEmqx()
    code = _issue(
        "pgr-issue-2",
        broker,
        emqx,
        mqtt_password="p4ssw0rd",
        hmac_key=b"z" * 32,
        host="broker.example.com",
        ca="ca-pem",
        label="Kid 2",
    )
    raw, host, port, _apn = devsetup.parse(code)
    bid, _bpw, bkey = devsetup.derive(raw)
    msg = broker.published[0]
    assert msg.topic == f"pager/boot/{bid}/down"
    decoded = devsetup.decrypt_bundle(bkey, msg.payload)
    assert decoded["id"] == "pgr-issue-2"
    assert decoded["pw"] == "p4ssw0rd"
    assert decoded["k"] == b"z" * 32
    assert decoded["host"] == host
    assert decoded["port"] == port
    assert decoded["label"] == "Kid 2"


def test_complete_clears_bundle_revokes_boot_user_and_deletes_setup_code():
    broker = FakeBrokerClient()
    emqx = _FakeEmqx()
    code = _issue("pgr-complete-1", broker, emqx)
    raw, *_ = devsetup.parse(code)
    bid, _bpw, _bkey = devsetup.derive(raw)
    broker.clear()

    devsetup.complete(bid, settings=_settings(), broker=broker, emqx=emqx)

    assert len(broker.published) == 1
    clear_msg = broker.published[0]
    assert clear_msg.topic == f"pager/boot/{bid}/down"
    assert clear_msg.payload == b""
    assert clear_msg.retain is True
    assert emqx.deleted == [f"boot-{bid}"]
    assert setup_codes_store.get(bid) is None


def test_complete_is_safe_to_call_twice():
    broker = FakeBrokerClient()
    emqx = _FakeEmqx()
    code = _issue("pgr-complete-2", broker, emqx)
    raw, *_ = devsetup.parse(code)
    bid, _bpw, _bkey = devsetup.derive(raw)

    devsetup.complete(bid, settings=_settings(), broker=broker, emqx=emqx)
    devsetup.complete(bid, settings=_settings(), broker=broker, emqx=emqx)  # no raise
    assert setup_codes_store.get(bid) is None


def test_expire_cleans_up_only_codes_past_their_expiresat():
    broker = FakeBrokerClient()
    emqx = _FakeEmqx()
    now = datetime.now(UTC)

    code_old = _issue("pgr-expire-old", broker, emqx, now=now - timedelta(minutes=20))
    code_fresh = _issue("pgr-expire-fresh", broker, emqx, now=now)
    bid_old, _, _ = devsetup.derive(devsetup.parse(code_old)[0])
    bid_fresh, _, _ = devsetup.derive(devsetup.parse(code_fresh)[0])
    broker.clear()

    count = devsetup.expire(now=now, settings=_settings(), broker=broker, emqx=emqx)

    assert count == 1
    assert setup_codes_store.get(bid_old) is None
    assert setup_codes_store.get(bid_fresh) is not None
    assert emqx.deleted == [f"boot-{bid_old}"]
    assert len(broker.published) == 1
    assert broker.published[0].topic == f"pager/boot/{bid_old}/down"


def test_expire_returns_zero_when_nothing_is_stale():
    broker = FakeBrokerClient()
    emqx = _FakeEmqx()
    now = datetime.now(UTC)
    _issue("pgr-expire-none", broker, emqx, now=now)
    count = devsetup.expire(now=now, settings=_settings(), broker=broker, emqx=emqx)
    assert count == 0


# ---------------------------------------------------------------------------
# app/store/setup_codes.py
# ---------------------------------------------------------------------------


def test_setup_codes_store_create_get_delete():
    now = datetime.now(UTC)
    setup_codes_store.create("bid-store-1", "pgr-store-1", now)
    fetched = setup_codes_store.get("bid-store-1")
    assert fetched is not None
    assert fetched.deviceId == "pgr-store-1"
    setup_codes_store.delete("bid-store-1")
    assert setup_codes_store.get("bid-store-1") is None


def test_setup_codes_store_get_missing_returns_none():
    assert setup_codes_store.get("bid-store-missing") is None


def test_setup_codes_store_list_expired():
    now = datetime.now(UTC)
    setup_codes_store.create("bid-store-old", "pgr-store-old", now - timedelta(minutes=1))
    setup_codes_store.create("bid-store-new", "pgr-store-new", now + timedelta(minutes=10))
    expired = {c.bid for c in setup_codes_store.list_expired(now)}
    assert "bid-store-old" in expired
    assert "bid-store-new" not in expired

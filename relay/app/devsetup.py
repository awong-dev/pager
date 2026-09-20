"""Setup codes and the bootstrap bundle -- `docs/DEVICE_PLAN.md` §3.1, §3.2
(`docs/DEVICE_TASKS.md` S2b.1).

**The problem this solves** (§3.0): a brand-new device has no wire, no extra
radio and no credential yet -- only a keyboard, a SIM and the one MQTT/TLS
stack it already has. The setup code (§3.1) is a ~40-character string a
person types once; everything else the device needs (its real MQTT
credential, its HMAC key, the broker's CA) travels over the device's own LTE
connection, encrypted under a key derived from that one typed string, so the
"downloading its config" step needs nothing more from the person than typing.

**Three derived values, one HKDF-SHA256 per label** (§3.2): `bid` (the
bootstrap MQTT username's suffix), `bpw` (that credential's password) and
`bkey` (the AES-256-GCM key the bundle is encrypted under). All three come
from the *same* 8-byte token so that "sees only one of them" (§3.2's "what
each party can see" table) is meaningful -- the broker sees `bid`/`bpw`, a
man-in-the-middle sees ciphertext, and only the person who read (or scanned)
the code has `bkey`. **The token itself is never written to Firestore** --
`setupCodes/{bid}` (`app/store/setup_codes.py`) holds only `bid`, the target
`deviceId` and an `expiresAt`, exactly what §3.2 step 3/5 says it may hold.

**`issue()`'s inputs are the plaintext secrets, not a device lookup.**
`docs/DEVICE_PLAN.md` §3.2 step 1 (generate `mqttPassword`/`hmacKey`, write
`devices/{d}` and `deviceSecrets/{d}`) is the caller's job (S2.2's extended
`POST /api/admin/devices`), for an unavoidable reason: the relay only ever
stores `mqttPasswordHash` (`app/routers/admin.py`'s `create_device`) --
nothing later can recover the plaintext MQTT password to put it in the
bundle. The one moment the plaintext exists is right after it is generated,
so `issue()` takes it (and the raw HMAC key, host/port/CA/label/flags/apn --
none of which `app/config.Settings` carries, since a household's own broker
host and CA are per-deployment values this task's `Files` list has nowhere to
add) as explicit parameters instead of reaching into a store for them.

**No new `broker.py` method.** The task description that motivated this
module speaks of "add retain support to `broker.publish_raw`" -- but
`app/broker.py`'s existing `publish(topic, payload, qos, retain)` already
takes `retain` as a plain bool and already sends it through to EMQX's REST
`/publish` (`{"topic":..., "payload":..., "qos":..., "retain":...}`); there is
no `publish_raw` method, and nothing needed adding to get a retained raw-bytes
publish. This module calls `BrokerClient.publish` directly and leaves
`app/broker.py` untouched.
"""

from __future__ import annotations

import os
import secrets
from datetime import UTC, datetime, timedelta
from typing import Any

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from pydantic import BaseModel, ConfigDict, field_validator, model_validator

from app import ca_resolve, wirecbor
from app.broker import BrokerClient
from app.config import Settings
from app.emqx_admin import EmqxAdmin
from app.store import setup_codes as setup_codes_store

# §3.1/H11: 8 random bytes (64 bits).
TOKEN_BYTES = 8
# 65 bits (64 + one padding zero bit) / 5 bits per Crockford symbol = 13.
TOKEN_CHARS = 13
CODE_GROUP_SIZE = 4
# §3.1: "Port defaults to 8883."
DEFAULT_PORT = 8883
# §3.1: "expires after 10 minutes or on first use."
EXPIRY_MINUTES = 10

# Crockford base32: 0-9 and A-Z minus I, L, O, U (case-insensitive on input;
# I/L/O are accepted on decode as common handwriting/typo stand-ins for
# 1/1/0, per Crockford's own spec -- U has no such stand-in and is simply
# rejected).
_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
_VALUE_OF: dict[str, int] = {c: i for i, c in enumerate(_ALPHABET)}
_VALUE_OF["O"] = 0
_VALUE_OF["I"] = 1
_VALUE_OF["L"] = 1


def _char_value(c: str) -> int:
    try:
        return _VALUE_OF[c]
    except KeyError:
        raise ValueError(f"invalid setup-code character: {c!r}") from None


# ---------------------------------------------------------------------------
# Luhn mod N (N=32) check character -- catches every single-character
# substitution and (per the algorithm's well-known property) adjacent
# transpositions, "so a typo is caught on the device before any radio comes
# up" (§3.1). https://en.wikipedia.org/wiki/Luhn_mod_N_algorithm
# ---------------------------------------------------------------------------


def _luhn_mod_n_generate(values: list[int], n: int = 32) -> int:
    factor = 2
    total = 0
    for v in reversed(values):
        addend = factor * v
        factor = 1 if factor == 2 else 2
        addend = (addend // n) + (addend % n)
        total += addend
    remainder = total % n
    return (n - remainder) % n


def _luhn_mod_n_validate(values_with_check: list[int], n: int = 32) -> bool:
    factor = 1
    total = 0
    for v in reversed(values_with_check):
        addend = factor * v
        factor = 1 if factor == 2 else 2
        addend = (addend // n) + (addend % n)
        total += addend
    return total % n == 0


def _encode_token(raw: bytes) -> str:
    """8 raw bytes -> 13 Crockford data characters (65 bits: 64 data bits
    plus one padding zero bit) plus one trailing check character."""
    if len(raw) != TOKEN_BYTES:
        raise ValueError(f"token must be {TOKEN_BYTES} bytes, got {len(raw)}")
    n = int.from_bytes(raw, "big") << 1  # pad to 65 bits
    values = [(n >> shift) & 0x1F for shift in range(60, -5, -5)]
    check = _luhn_mod_n_generate(values)
    return "".join(_ALPHABET[v] for v in values) + _ALPHABET[check]


def _decode_token(token_str: str) -> bytes:
    """Inverse of `_encode_token`. Raises `ValueError` on a wrong length, an
    invalid character or a check-character mismatch (a typo)."""
    if len(token_str) != TOKEN_CHARS + 1:
        raise ValueError(
            f"setup code token must be {TOKEN_CHARS + 1} characters, got {len(token_str)}"
        )
    values = [_char_value(c) for c in token_str]
    if not _luhn_mod_n_validate(values):
        raise ValueError("setup code check character mismatch (typo?)")
    n = 0
    for v in values[:-1]:
        n = (n << 5) | v
    n >>= 1  # drop the padding bit
    return n.to_bytes(TOKEN_BYTES, "big")


def token() -> tuple[bytes, str]:
    """Generates a fresh setup-code token: `TOKEN_BYTES` random bytes (H11),
    and its 14-character Crockford-base32 encoding (13 data characters + 1
    check character), ungrouped -- `format_code` adds the hyphen groups and
    the `@ host[:port][;apn=...]` suffix. Returns `(raw_bytes, token_str)`;
    callers that only want a fresh code use `token_str`, `issue()` uses
    `raw_bytes` to `derive()` from."""
    raw = secrets.token_bytes(TOKEN_BYTES)
    return raw, _encode_token(raw)


def format_code(token_str: str, *, host: str, port: int = DEFAULT_PORT, apn: str | None = None) -> str:
    """§3.1's full displayed string: the token in groups of four, ` @ ` the
    broker host (port suffix only when it isn't the default), and an
    optional `;apn=...`."""
    groups = [token_str[i : i + CODE_GROUP_SIZE] for i in range(0, len(token_str), CODE_GROUP_SIZE)]
    host_part = host if port == DEFAULT_PORT else f"{host}:{port}"
    code = f"{'-'.join(groups)} @ {host_part}"
    if apn:
        code += f";apn={apn}"
    return code


def parse(code: str) -> tuple[bytes, str, int, str | None]:
    """Inverse of `format_code`/`token()`+derivation input: accepts spaces,
    hyphens and mixed case in the *token* portion (the part before `@`) --
    the host/APN portion is taken verbatim apart from surrounding whitespace,
    since a broker hostname legitimately contains hyphens and dots. Returns
    `(token_bytes, host, port, apn)`; raises `ValueError` on anything
    malformed, including a bad check character."""
    if "@" not in code:
        raise ValueError("setup code is missing '@ host'")
    token_part, rest = code.split("@", 1)
    token_str = "".join(ch for ch in token_part if not ch.isspace() and ch != "-").upper()
    raw = _decode_token(token_str)

    rest = rest.strip()
    apn: str | None = None
    if ";apn=" in rest:
        rest, apn = rest.split(";apn=", 1)
        rest = rest.strip()
        apn = apn.strip() or None

    if ":" in rest:
        host, port_str = rest.rsplit(":", 1)
        try:
            port = int(port_str.strip())
        except ValueError as exc:
            raise ValueError(f"invalid port in setup code: {port_str!r}") from exc
    else:
        host, port = rest, DEFAULT_PORT
    host = host.strip()
    if not host:
        raise ValueError("setup code is missing a host")
    return raw, host, port, apn


# ---------------------------------------------------------------------------
# derive() -- §3.2: three values from one token, one HKDF-SHA256 label each.
# ---------------------------------------------------------------------------


def _hkdf(token_bytes: bytes, info: bytes, length: int = 32) -> bytes:
    return HKDF(algorithm=hashes.SHA256(), length=length, salt=None, info=info).derive(token_bytes)


def derive(token_bytes: bytes) -> tuple[str, str, bytes]:
    """`(bid, bpw, bkey)` -- §3.2: `bid = hex(HKDF(token,"id"))[:12]`,
    `bpw = base64(HKDF(token,"pw"))`, `bkey = HKDF(token,"bundle")` (32 B)."""
    bid = _hkdf(token_bytes, b"id").hex()[:12]
    bpw = _b64url(_hkdf(token_bytes, b"pw"))
    bkey = _hkdf(token_bytes, b"bundle")
    return bid, bpw, bkey


def _b64url(raw: bytes) -> str:
    import base64

    return base64.urlsafe_b64encode(raw).decode("ascii")


def boot_username(bid: str) -> str:
    """`docs/DEVICE_PLAN.md` §3.2 step 3's `boot-{bid}` -- kept in sync with
    `app/emqx_admin.py`'s own (module-private) `_boot_username`."""
    return f"boot-{bid}"


def boot_topic_down(bid: str) -> str:
    return f"pager/boot/{bid}/down"


# ---------------------------------------------------------------------------
# bundle() -- §3.2 step 4, §8 item 10's boot keymap (0, 1, 30-37).
# ---------------------------------------------------------------------------


class BootstrapDevice(BaseModel):
    """The bundle's plaintext fields (`docs/DEVICE_PLAN.md` §3.2 step 4):
    `{"v":1,"id":...,"pw":...,"k":...,"host":...,"port":...,"ca_url":...,
    "ca_sha":...,"flags":...,"label":...}`, plus the rarely-needed `apn`.
    `k` is the raw 32-byte HMAC key -- `app/wirecbor.py`'s CBOR encoder
    writes `bytes` values as CBOR byte strings, which is what the device
    needs, not base64 text.

    docs/V02_DESIGN.md §4.4: **the CA never travels inline again.** `ca_url`
    + `ca_sha` (a pointer, fetched and hash-checked) replace the old `ca`
    field (a full PEM, which does not fit the modem library's 1540-byte
    receive buffer for a Let's Encrypt/Google root -- `CA_TRUST_PLAN.md`
    §3.4). Both absent means unpinned; `_check_ca_pair` below enforces they
    are never given one without the other, since a `ca_url` a device cannot
    hash-check is worse than no CA at all."""

    model_config = ConfigDict(extra="forbid")

    id: str
    pw: str
    k: bytes
    host: str
    port: int = DEFAULT_PORT
    ca_url: str | None = None
    ca_sha: bytes | None = None
    flags: int = 0
    label: str
    apn: str | None = None

    @field_validator("ca_sha")
    @classmethod
    def _check_ca_sha_len(cls, value: bytes | None) -> bytes | None:
        if value is not None and len(value) != 32:
            raise ValueError("ca_sha must be a 32-byte SHA-256 digest")
        return value

    @model_validator(mode="after")
    def _check_ca_pair(self) -> BootstrapDevice:
        if (self.ca_url is None) != (self.ca_sha is None):
            raise ValueError("ca_url and ca_sha must be both present or both absent")
        return self


def bundle(device: BootstrapDevice, bkey: bytes) -> bytes:
    """CBOR-encodes `device` under boot keys 0 (`v`), 1 (`id`), 30-33/35-37
    (`pw,k,host,port,flags,label,apn`) plus the v0.2 pointer keys 40-41
    (`ca_url,ca_sha`) -- `app/wirecbor.KEYMAP` already assigns exactly those
    numbers to those names (`docs/PROTOCOL.md` §10) -- then AES-256-GCM-
    encrypts it under `bkey` with a fresh random 12-byte nonce. Returns
    `nonce ‖ ciphertext ‖ tag` raw bytes (`cryptography`'s `AESGCM.encrypt`
    already appends the 16-byte tag to the ciphertext, so prepending the
    nonce is the only assembly needed)."""
    plain_obj: dict[str, Any] = {
        "v": 1,
        "id": device.id,
        "pw": device.pw,
        "k": device.k,
        "host": device.host,
        "port": device.port,
        "flags": device.flags,
        "label": device.label,
    }
    if device.ca_url is not None:
        plain_obj["ca_url"] = device.ca_url
        plain_obj["ca_sha"] = device.ca_sha
    if device.apn is not None:
        plain_obj["apn"] = device.apn
    plaintext = wirecbor.encode(plain_obj)
    nonce = os.urandom(12)
    ct = AESGCM(bkey).encrypt(nonce, plaintext, None)
    return nonce + ct


def decrypt_bundle(bkey: bytes, blob: bytes) -> dict[str, Any]:
    """Inverse of `bundle()` -- the device's own side of §3.2 step 4, used
    here only by tests/the vector (the relay itself never needs to decrypt
    what it just encrypted). Raises `cryptography.exceptions.InvalidTag` on
    a wrong key or corrupted ciphertext."""
    nonce, ct = blob[:12], blob[12:]
    plaintext = AESGCM(bkey).decrypt(nonce, ct, None)
    return wirecbor.decode(plaintext)


# ---------------------------------------------------------------------------
# issue() / complete() / expire() -- §3.2 steps 1-5, "Relay, on
# pager/boot/+/up", and jobs.tick's stale-code cleanup.
# ---------------------------------------------------------------------------


def issue(
    device_id: str,
    *,
    mqtt_password: str,
    hmac_key: bytes,
    host: str,
    ca_pem: str | None,
    label: str,
    port: int = DEFAULT_PORT,
    flags: int = 0,
    apn: str | None = None,
    settings: Settings | None = None,
    broker: BrokerClient | None = None,
    emqx: EmqxAdmin | None = None,
    now: datetime | None = None,
) -> str:
    """§3.2 steps 2-5 (step 1 -- generating `mqttPassword`/`hmacKey` and
    writing `devices/{d}`/`deviceSecrets/{d}` -- is the caller's job; see the
    module docstring for why the plaintext values must come in as
    parameters). Pushes the bootstrap credential to the broker, publishes
    the encrypted bundle retained to `pager/boot/{bid}/down`, records
    `setupCodes/{bid}` (never the token), and returns the setup code once.

    `ca_pem` (docs/V02_DESIGN.md §4.4): the CA PEM text to pin, or `None`/
    empty for an unpinned deployment (both bundle fields absent -- see
    `BootstrapDevice`'s docstring). When given, this resolves to a pointer
    (`ca_resolve.ca_pointer`) rather than travelling inline; that call
    raises `ca_resolve.PublicBaseUrlRequired` if `settings.public_base_url`
    is empty, which propagates out of `issue()` uncaught -- the caller
    (`app/routers/admin.py`) turns it into a 500 rather than this module
    silently sending an unpinned bundle for a deployment that thinks it has
    pinned one."""
    settings = settings if settings is not None else Settings.from_env()
    broker = broker if broker is not None else BrokerClient(settings)
    emqx = emqx if emqx is not None else EmqxAdmin(settings)

    raw, token_str = token()
    bid, bpw, bkey = derive(raw)

    emqx.ensure_boot_user(bid, bpw)

    ca_url: str | None = None
    ca_sha: bytes | None = None
    if ca_pem:
        ca_url, ca_sha = ca_resolve.ca_pointer(ca_pem, settings)

    device = BootstrapDevice(
        id=device_id,
        pw=mqtt_password,
        k=hmac_key,
        host=host,
        port=port,
        ca_url=ca_url,
        ca_sha=ca_sha,
        flags=flags,
        label=label,
        apn=apn,
    )
    blob = bundle(device, bkey)
    broker.publish(boot_topic_down(bid), blob, qos=1, retain=True)

    now = now if now is not None else datetime.now(UTC)
    setup_codes_store.create(bid, device_id, now + timedelta(minutes=EXPIRY_MINUTES))

    return format_code(token_str, host=host, port=port, apn=apn)


def _clear_and_revoke(bid: str, broker: BrokerClient, emqx: EmqxAdmin) -> None:
    # §3.2 "Relay, on pager/boot/+/up": clear the retained bundle, then
    # revoke the bootstrap credential -- in that order, so a retry between
    # the two calls (either can fail independently; `BrokerClient.publish`/
    # `EmqxAdmin.delete_user` never raise) never leaves a still-readable
    # bundle behind a still-live credential.
    broker.publish(boot_topic_down(bid), b"", qos=1, retain=True)
    emqx.delete_user(boot_username(bid))


def complete(
    bid: str,
    *,
    settings: Settings | None = None,
    broker: BrokerClient | None = None,
    emqx: EmqxAdmin | None = None,
) -> None:
    """§3.2 "Relay, on `pager/boot/+/up`" (the `{v:1, ok:1}` message itself
    is matched by the caller -- a later webhook-dispatch task, outside this
    module -- which then calls this with the `bid` from the topic): clear the
    retained bundle, delete the bootstrap credential, delete
    `setupCodes/{bid}`. Safe to call more than once (every step here is
    itself idempotent -- an empty retained publish, `delete_user`'s 404-is-
    success, a delete of an already-absent document)."""
    settings = settings if settings is not None else Settings.from_env()
    broker = broker if broker is not None else BrokerClient(settings)
    emqx = emqx if emqx is not None else EmqxAdmin(settings)
    _clear_and_revoke(bid, broker, emqx)
    setup_codes_store.delete(bid)


def expire(
    *,
    now: datetime | None = None,
    settings: Settings | None = None,
    broker: BrokerClient | None = None,
    emqx: EmqxAdmin | None = None,
) -> int:
    """§3.2: "`jobs.tick` (every 5 min) expires stale codes the same way [as
    `complete`]." `app/jobs.py` (its `tick()`) is outside this task's `Files`
    list, so this is a standalone, importable function -- a future task wires
    it into that scheduler's loop; nothing here assumes it is. Returns how
    many codes were expired."""
    now = now if now is not None else datetime.now(UTC)
    settings = settings if settings is not None else Settings.from_env()
    broker = broker if broker is not None else BrokerClient(settings)
    emqx = emqx if emqx is not None else EmqxAdmin(settings)
    expired = setup_codes_store.list_expired(now)
    for code in expired:
        _clear_and_revoke(code.bid, broker, emqx)
        setup_codes_store.delete(code.bid)
    return len(expired)

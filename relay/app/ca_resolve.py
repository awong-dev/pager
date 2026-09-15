"""Broker CA resolution -- docs/DEVICE_PLAN.md §3.3 (docs/DEVICE_TASKS.md
S2b.2).

**Why.** The bootstrap hop (§3.2) authenticates by HMAC, not TLS, so it
needs no CA. The production session does pin one (`net.cpp`'s hardcoded
DigiCert root today, being replaced by whatever this module resolves), and
since every household's broker can chain to a different root, the relay has
to figure out *which* CA to hand a device before it can issue that device's
bootstrap bundle. `BROKER_CA_PEM` in config wins outright when a deployment
already knows it (its own Terraform set it deliberately); otherwise this
module connects to the broker once and works out the CA itself, so a wrong
guess is visible on *Admin -> Settings* (`brokerCaSubject`) before a device
is issued.

**What this task's `Files` list could not wire in.** `GET
/api/admin/settings` already exists (`app/routers/admin.py`), but that file
(and `app/store/devices.py`) is being edited concurrently by S2.2 as of this
change and is deliberately not in this task's `Files` list, so
`brokerCaSubject` is not yet plumbed into that response body. Likewise,
"resolved once at startup" (§3.3) implies a call from `app/main.py`'s
lifespan, which is also not in this task's `Files` list. Both are one-line
follow-ups once those files are free:

    # app/main.py, inside `lifespan()`, after `settings = ...`:
    from app import ca_resolve
    ca_resolve.resolve_broker_ca(settings)

    # app/routers/admin.py, in `get_settings()`'s response model:
    brokerCaSubject: str | None = ca_resolve.get_broker_ca_subject()

Until then, `resolve_broker_ca()`/`get_broker_ca_subject()`/
`get_broker_ca_pem()` below are fully functional and unit-tested but nothing
in the running app calls them yet.
"""

from __future__ import annotations

import functools
import logging
import socket
import ssl
from collections.abc import Sequence
from dataclasses import dataclass

import certifi
from cryptography import x509
from cryptography.hazmat.primitives import serialization

from app.config import Settings

logger = logging.getLogger("relay.ca_resolve")

# docs/PROTOCOL.md §6.1 / DEVICE_PLAN.md §3.3: the broker's TLS (production,
# non-bootstrap) listener.
BROKER_TLS_PORT = 8883
_CONNECT_TIMEOUT_S = 5.0


@dataclass(frozen=True, slots=True)
class BrokerCa:
    """The resolved broker CA. `pem` is what belongs in a bootstrap bundle's
    `ca` field (DEVICE_PLAN.md §3.4, NVS-capped at 4 kB there); `subject` is
    the human-readable RFC 4514 name `Admin -> Settings` shows as
    `brokerCaSubject` so a wrong guess is visible before a device is
    issued."""

    pem: str
    subject: str


# Module-level cache: resolution happens once (§3.3: "resolved once at
# startup"), not on every request that wants the CA.
_cached: BrokerCa | None = None


def reset_cache() -> None:
    """Test-only. Each test that exercises `resolve_broker_ca` wants to
    start from "nothing resolved yet" rather than leak a previous test's
    (or the importing process's) result -- also drops the memoized certifi
    bundle (`_load_ca_roots`) so a test that monkeypatches `certifi.where`
    to a fixture bundle does not see an earlier test's parsed roots."""
    global _cached
    _cached = None
    _load_ca_roots.cache_clear()


def get_broker_ca_pem() -> str | None:
    """What a bootstrap bundle's `ca` field should be filled with (empty/
    `None` if resolution never ran or failed -- the device then reports
    "broker certificate not trusted", per this task's `Do`)."""
    return _cached.pem if _cached else None


def get_broker_ca_subject() -> str | None:
    """What `GET /api/admin/settings`'s `brokerCaSubject` should be, once
    wired -- see this module's top docstring."""
    return _cached.subject if _cached else None


def resolve_broker_ca(settings: Settings) -> BrokerCa | None:
    """Resolves and caches the broker CA, per §3.3's precedence:
    `settings.broker_ca_pem` wins unconditionally (no network involved --
    a deployment's Terraform already knows its own broker's CA); otherwise
    connect to `settings.broker_host:8883`, take the served certificate
    chain, and find the root (§3.3: "if the root is not served, find it in
    certifi by issuer name"). On any failure, logs a warning, caches
    nothing, and returns `None` -- callers must treat that as "no CA",
    never as "keep whatever was cached before" (a broker CA rotation that
    breaks resolution should surface as empty, not stale)."""
    global _cached

    if settings.broker_ca_pem:
        cert = _try_parse_pem(settings.broker_ca_pem)
        if cert is None:
            logger.warning(
                "BROKER_CA_PEM is not a parseable certificate; leaving broker ca empty"
            )
            _cached = None
            return None
        _cached = BrokerCa(pem=settings.broker_ca_pem, subject=_subject_str(cert))
        return _cached

    try:
        chain_der = fetch_served_chain(settings.broker_host, BROKER_TLS_PORT)
    except OSError as exc:
        logger.warning(
            "broker CA auto-resolve: could not connect to %s:%d (%s)",
            settings.broker_host,
            BROKER_TLS_PORT,
            exc,
        )
        _cached = None
        return None

    resolved = resolve_ca_from_chain(chain_der)
    if resolved is None:
        logger.warning(
            "broker CA auto-resolve: no certifi root matches the chain served by %s:%d",
            settings.broker_host,
            BROKER_TLS_PORT,
        )
    _cached = resolved
    return resolved


def fetch_served_chain(host: str, port: int, timeout: float = _CONNECT_TIMEOUT_S) -> list[bytes]:
    """Connects to `host:port` with certificate verification disabled --
    there is nothing to verify against yet, resolving the CA is the point --
    and returns the DER-encoded certificate chain the server sent, leaf
    first.

    Python's stdlib only exposes the *full* served chain from 3.13's
    `SSLSocket.get_unverified_chain()`; this project's floor is 3.12
    (`pyproject.toml`), so on older stdlibs this falls back to the leaf
    alone via `getpeercert(binary_form=True)`. That is sufficient for every
    row in DEVICE_PLAN.md §3.3's table where the leaf's issuer is itself a
    certifi root (no intermediate); a leaf whose issuer is an intermediate
    that itself is not served will not resolve on 3.12, and resolution then
    fails closed (see `resolve_broker_ca`'s docstring)."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    with (
        socket.create_connection((host, port), timeout=timeout) as raw,
        context.wrap_socket(raw, server_hostname=host) as tls,
    ):
        get_chain = getattr(tls, "get_unverified_chain", None)
        if callable(get_chain):
            try:
                chain = get_chain() or []
            except ssl.SSLError:
                chain = []
            if chain:
                return list(chain)
        leaf = tls.getpeercert(binary_form=True)
        return [leaf] if leaf else []


def resolve_ca_from_chain(chain_der: Sequence[bytes]) -> BrokerCa | None:
    """Pure resolution logic, deliberately separate from `fetch_served_chain`
    so tests can hand it a fixture chain without a live TLS connection.

    Walks the served chain from the leaf. If the served chain already
    contains a self-signed certificate (subject == issuer -- a root the
    server chose to serve), that is the CA, no lookup needed. Otherwise
    takes the *topmost* served certificate's issuer name and looks for a
    certifi root whose subject matches it (§3.3: "if the root is not
    served, find it in certifi by issuer name")."""
    certs = _parse_der_chain(chain_der)
    if not certs:
        return None

    for cert in certs:
        if cert.subject == cert.issuer:
            return BrokerCa(pem=_to_pem(cert), subject=_subject_str(cert))

    topmost = certs[-1]
    root = _find_certifi_root_by_subject(topmost.issuer)
    if root is None:
        return None
    return BrokerCa(pem=_to_pem(root), subject=_subject_str(root))


def _parse_der_chain(chain_der: Sequence[bytes]) -> list[x509.Certificate]:
    certs = []
    for der in chain_der:
        try:
            certs.append(x509.load_der_x509_certificate(der))
        except ValueError:
            continue
    return certs


def _try_parse_pem(pem: str) -> x509.Certificate | None:
    try:
        return x509.load_pem_x509_certificate(pem.encode())
    except ValueError:
        return None


def _find_certifi_root_by_subject(subject: x509.Name) -> x509.Certificate | None:
    for root in _load_ca_roots():
        if root.subject == subject:
            return root
    return None


@functools.lru_cache(maxsize=1)
def _load_ca_roots() -> tuple[x509.Certificate, ...]:
    """The certifi CA bundle, parsed once. `certifi.where()` is a plain
    file path (a `cacert.pem` bundled with the package); tests override it
    by monkeypatching `certifi.where` to point at a fixture bundle so a
    self-signed test root can act as "the" certifi root without needing a
    real public CA."""
    with open(certifi.where(), "rb") as f:
        data = f.read()
    roots = []
    for block in _split_pem_certificates(data):
        try:
            roots.append(x509.load_pem_x509_certificate(block))
        except ValueError:
            continue
    return tuple(roots)


def _split_pem_certificates(data: bytes) -> list[bytes]:
    blocks: list[bytes] = []
    current: list[bytes] = []
    in_block = False
    for line in data.splitlines(keepends=True):
        if line.startswith(b"-----BEGIN CERTIFICATE-----"):
            in_block = True
            current = [line]
        elif line.startswith(b"-----END CERTIFICATE-----"):
            current.append(line)
            blocks.append(b"".join(current))
            in_block = False
        elif in_block:
            current.append(line)
    return blocks


def _to_pem(cert: x509.Certificate) -> str:
    return cert.public_bytes(serialization.Encoding.PEM).decode()


def _subject_str(cert: x509.Certificate) -> str:
    return cert.subject.rfc4514_string()

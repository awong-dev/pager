"""docs/DEVICE_TASKS.md S2b.2: `app/ca_resolve.py`'s config-wins precedence
and the served-chain/certifi-lookup resolution logic, against a self-signed
chain fixture (no live broker, no network)."""

from __future__ import annotations

import datetime
from collections.abc import Iterator
from pathlib import Path

import certifi
import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from app import ca_resolve
from app.config import Settings


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "test-webhook-key",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


def _self_signed_cert(name: str, key: ec.EllipticCurvePrivateKey) -> x509.Certificate:
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.datetime.now(datetime.UTC)
    return (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )


def _leaf_cert(
    name: str,
    key: ec.EllipticCurvePrivateKey,
    issuer_cert: x509.Certificate,
    issuer_key: ec.EllipticCurvePrivateKey,
) -> x509.Certificate:
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.datetime.now(datetime.UTC)
    return (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .sign(issuer_key, hashes.SHA256())
    )


def _der(cert: x509.Certificate) -> bytes:
    return cert.public_bytes(serialization.Encoding.DER)


def _pem(cert: x509.Certificate) -> str:
    return cert.public_bytes(serialization.Encoding.PEM).decode()


@pytest.fixture()
def chain_fixture() -> tuple[x509.Certificate, x509.Certificate]:
    """A self-signed root + a leaf it issued, exactly §3.3's "EMQX open
    source, untouched defaults" row (a self-signed test CA)."""
    root_key = ec.generate_private_key(ec.SECP256R1())
    root_cert = _self_signed_cert("Test Root CA", root_key)
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    leaf_cert = _leaf_cert("broker.test", leaf_key, root_cert, root_key)
    return root_cert, leaf_cert


@pytest.fixture(autouse=True)
def _reset_cache() -> Iterator[None]:
    ca_resolve.reset_cache()
    yield
    ca_resolve.reset_cache()


def test_resolve_ca_from_chain_finds_root_via_certifi(
    chain_fixture: tuple[x509.Certificate, x509.Certificate],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    root_cert, leaf_cert = chain_fixture
    bundle = tmp_path / "cacert.pem"
    bundle.write_text(_pem(root_cert))
    monkeypatch.setattr(certifi, "where", lambda: str(bundle))

    resolved = ca_resolve.resolve_ca_from_chain([_der(leaf_cert)])

    assert resolved is not None
    assert resolved.subject == "CN=Test Root CA"
    assert x509.load_pem_x509_certificate(resolved.pem.encode()) == root_cert


def test_resolve_ca_from_chain_root_served_directly(
    chain_fixture: tuple[x509.Certificate, x509.Certificate],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """If the server serves the root itself, no certifi lookup is needed --
    point `certifi.where()` at an empty bundle to prove it."""
    root_cert, leaf_cert = chain_fixture
    bundle = tmp_path / "cacert.pem"
    bundle.write_text("")
    monkeypatch.setattr(certifi, "where", lambda: str(bundle))

    resolved = ca_resolve.resolve_ca_from_chain([_der(leaf_cert), _der(root_cert)])

    assert resolved is not None
    assert resolved.subject == "CN=Test Root CA"


def test_resolve_ca_from_chain_no_certifi_match_returns_none(
    chain_fixture: tuple[x509.Certificate, x509.Certificate],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _root_cert, leaf_cert = chain_fixture
    bundle = tmp_path / "cacert.pem"
    bundle.write_text("")  # no roots at all -- nothing can match
    monkeypatch.setattr(certifi, "where", lambda: str(bundle))

    assert ca_resolve.resolve_ca_from_chain([_der(leaf_cert)]) is None


def test_resolve_ca_from_chain_empty_chain_returns_none() -> None:
    assert ca_resolve.resolve_ca_from_chain([]) is None


def test_resolve_broker_ca_config_pem_wins_without_network(
    chain_fixture: tuple[x509.Certificate, x509.Certificate],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    root_cert, _leaf_cert = chain_fixture
    settings = make_settings(broker_ca_pem=_pem(root_cert), broker_host="unused.invalid")

    def _boom(host: str, port: int, timeout: float = 5.0) -> list[bytes]:
        raise AssertionError("fetch_served_chain must not be called when BROKER_CA_PEM is set")

    monkeypatch.setattr(ca_resolve, "fetch_served_chain", _boom)

    resolved = ca_resolve.resolve_broker_ca(settings)

    assert resolved is not None
    assert resolved.subject == "CN=Test Root CA"
    assert ca_resolve.get_broker_ca_subject() == "CN=Test Root CA"
    assert ca_resolve.get_broker_ca_pem() == _pem(root_cert)


def test_resolve_broker_ca_invalid_config_pem_leaves_ca_empty() -> None:
    settings = make_settings(broker_ca_pem="not a certificate", broker_host="unused.invalid")

    resolved = ca_resolve.resolve_broker_ca(settings)

    assert resolved is None
    assert ca_resolve.get_broker_ca_subject() is None
    assert ca_resolve.get_broker_ca_pem() is None


def test_resolve_broker_ca_falls_back_to_served_chain(
    chain_fixture: tuple[x509.Certificate, x509.Certificate],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    root_cert, leaf_cert = chain_fixture
    bundle = tmp_path / "cacert.pem"
    bundle.write_text(_pem(root_cert))
    monkeypatch.setattr(certifi, "where", lambda: str(bundle))
    monkeypatch.setattr(
        ca_resolve,
        "fetch_served_chain",
        lambda host, port, timeout=5.0: [_der(leaf_cert)],
    )
    settings = make_settings(broker_ca_pem=None, broker_host="broker.example.invalid")

    resolved = ca_resolve.resolve_broker_ca(settings)

    assert resolved is not None
    assert resolved.subject == "CN=Test Root CA"
    assert ca_resolve.get_broker_ca_subject() == "CN=Test Root CA"


def test_resolve_broker_ca_connect_failure_leaves_ca_empty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def _refuse(host: str, port: int, timeout: float = 5.0) -> list[bytes]:
        raise ConnectionRefusedError("no broker listening")

    monkeypatch.setattr(ca_resolve, "fetch_served_chain", _refuse)
    settings = make_settings(broker_ca_pem=None, broker_host="broker.example.invalid")

    resolved = ca_resolve.resolve_broker_ca(settings)

    assert resolved is None
    assert ca_resolve.get_broker_ca_subject() is None
    assert ca_resolve.get_broker_ca_pem() is None


def test_resolve_broker_ca_no_match_leaves_ca_empty(
    chain_fixture: tuple[x509.Certificate, x509.Certificate],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _root_cert, leaf_cert = chain_fixture
    bundle = tmp_path / "cacert.pem"
    bundle.write_text("")  # unrelated real-world bundle: no match
    monkeypatch.setattr(certifi, "where", lambda: str(bundle))
    monkeypatch.setattr(
        ca_resolve,
        "fetch_served_chain",
        lambda host, port, timeout=5.0: [_der(leaf_cert)],
    )
    settings = make_settings(broker_ca_pem=None, broker_host="broker.example.invalid")

    resolved = ca_resolve.resolve_broker_ca(settings)

    assert resolved is None
    assert ca_resolve.get_broker_ca_subject() is None

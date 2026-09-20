"""`GET /ca/{sha256hex}.pem` -- docs/V02_DESIGN.md §4.4 / docs/CA_TRUST_PLAN.md
§3.4. Public, unauthenticated: no admin header, no webhook key, anywhere in
this file."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from app.store import cas as cas_store
from tests.fake_transport import FakeBrokerClient

_SHA_HEX = "ab" * 32
_PEM = "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n"


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


@pytest.fixture
def client() -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        yield c


def test_get_ca_pem_serves_a_known_hash_no_auth_needed(client: TestClient):
    cas_store.remember(_SHA_HEX, _PEM)

    resp = client.get(f"/ca/{_SHA_HEX}.pem")

    assert resp.status_code == 200
    assert resp.text == _PEM
    assert resp.headers["content-type"].startswith("text/plain")
    assert "immutable" in resp.headers["cache-control"]
    assert "max-age=31536000" in resp.headers["cache-control"]


def test_get_ca_pem_404s_an_unknown_hash(client: TestClient):
    resp = client.get(f"/ca/{'cd' * 32}.pem")
    assert resp.status_code == 404


def test_get_ca_pem_404s_a_malformed_hash_rather_than_500(client: TestClient):
    for bad in ("not-hex.pem", "ab" * 31 + ".pem", "AB" * 32 + ".pem", "../../etc/passwd.pem"):
        resp = client.get(f"/ca/{bad}")
        assert resp.status_code == 404, bad

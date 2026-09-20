"""`app.store.cas` -- docs/V02_DESIGN.md §4.4's `cas/{sha256hex}` store."""

from __future__ import annotations

from app.store import cas as cas_store

_SHA_HEX = "ab" * 32
_PEM = "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n"


def test_remember_then_get_pem_round_trips():
    cas_store.remember(_SHA_HEX, _PEM)
    assert cas_store.get_pem(_SHA_HEX) == _PEM


def test_get_pem_missing_returns_none():
    assert cas_store.get_pem("cd" * 32) is None


def test_remember_is_idempotent():
    cas_store.remember(_SHA_HEX, _PEM)
    cas_store.remember(_SHA_HEX, _PEM)  # same content, same hash -- no error
    assert cas_store.get_pem(_SHA_HEX) == _PEM

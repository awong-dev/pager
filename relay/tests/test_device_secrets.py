"""`app/store/device_secrets.py` -- docs/DEVICE_PLAN.md §2.5, §2.6."""

from __future__ import annotations

import base64

import pytest

from app.store import device_secrets as device_secrets_store


def test_create_returns_the_expected_shape():
    secret = device_secrets_store.create(
        "pgr-secret-1", hmac_key=b"k" * 32, mqtt_password_hash="hash1"
    )
    assert secret.id == "pgr-secret-1"
    assert secret.hmacKey == b"k" * 32
    assert secret.mqttPasswordHash == "hash1"
    assert secret.upN == 0
    assert secret.upBits == 0
    assert secret.downN == 0
    assert secret.sigFailures == 0
    assert secret.createdAt is not None
    assert secret.rotatedAt is None


def test_hmac_key_round_trips_through_base64_storage():
    key = bytes(range(32))
    device_secrets_store.create("pgr-secret-2", hmac_key=key, mqtt_password_hash="h")
    fetched = device_secrets_store.get("pgr-secret-2")
    assert fetched is not None
    assert fetched.hmacKey == key
    # The document itself stores base64 text (DEVICE_PLAN.md §2.6), not raw
    # bytes -- pin the encoding, not just the round trip.
    from app.db.firestore import get_db

    raw = get_db().collection("deviceSecrets").document("pgr-secret-2").get().to_dict()
    assert raw is not None
    assert base64.b64decode(raw["hmacKey"]) == key


def test_get_missing_device_returns_none():
    assert device_secrets_store.get("pgr-secret-missing") is None


def test_rotate_replaces_secrets_and_zeroes_counters():
    device_secrets_store.create("pgr-secret-3", hmac_key=b"a" * 32, mqtt_password_hash="old")
    device_secrets_store.accept_up_n("pgr-secret-3", 5)
    device_secrets_store.next_down_n("pgr-secret-3")
    device_secrets_store.bump_sig_failures("pgr-secret-3")

    rotated = device_secrets_store.rotate("pgr-secret-3", b"b" * 32, "new")
    assert rotated.hmacKey == b"b" * 32
    assert rotated.mqttPasswordHash == "new"
    assert rotated.upN == 0
    assert rotated.upBits == 0
    assert rotated.downN == 0
    assert rotated.sigFailures == 0
    assert rotated.rotatedAt is not None


def test_rotate_missing_device_raises_key_error():
    with pytest.raises(KeyError):
        device_secrets_store.rotate("pgr-secret-nope", b"x" * 32, "h")


def test_delete_removes_the_document():
    device_secrets_store.create("pgr-secret-4", hmac_key=b"c" * 32, mqtt_password_hash="h")
    device_secrets_store.delete("pgr-secret-4")
    assert device_secrets_store.get("pgr-secret-4") is None


# ---------------------------------------------------------------------------
# accept_up_n -- the §2.5 replay window.
# ---------------------------------------------------------------------------


def test_accept_up_n_accepts_strictly_increasing_values():
    device_secrets_store.create("pgr-win-1", hmac_key=b"k" * 32, mqtt_password_hash="h")
    assert device_secrets_store.accept_up_n("pgr-win-1", 1) is True
    assert device_secrets_store.accept_up_n("pgr-win-1", 2) is True
    assert device_secrets_store.accept_up_n("pgr-win-1", 3) is True
    secret = device_secrets_store.get("pgr-win-1")
    assert secret is not None
    assert secret.upN == 3


def test_accept_up_n_rejects_an_exact_replay_of_the_current_top():
    device_secrets_store.create("pgr-win-2", hmac_key=b"k" * 32, mqtt_password_hash="h")
    assert device_secrets_store.accept_up_n("pgr-win-2", 10) is True
    assert device_secrets_store.accept_up_n("pgr-win-2", 10) is False
    secret = device_secrets_store.get("pgr-win-2")
    assert secret is not None
    assert secret.upN == 10


def test_accept_up_n_fills_a_gap_left_by_reordering_exactly_once():
    """The broker's webhook push is at-least-once and may reorder retries
    (DEVICE_PLAN.md §2.5) -- an out-of-order arrival within the window is
    accepted once, and a duplicate of it afterwards is rejected."""
    device_secrets_store.create("pgr-win-3", hmac_key=b"k" * 32, mqtt_password_hash="h")
    assert device_secrets_store.accept_up_n("pgr-win-3", 1) is True
    assert device_secrets_store.accept_up_n("pgr-win-3", 3) is True  # 2 skipped
    assert device_secrets_store.accept_up_n("pgr-win-3", 2) is True  # gap filled
    assert device_secrets_store.accept_up_n("pgr-win-3", 2) is False  # replay of the fill
    assert device_secrets_store.accept_up_n("pgr-win-3", 1) is False  # replay of the original


def test_accept_up_n_rejects_a_value_older_than_the_64_wide_window():
    device_secrets_store.create("pgr-win-4", hmac_key=b"k" * 32, mqtt_password_hash="h")
    device_secrets_store.accept_up_n("pgr-win-4", 1000)
    # 1000 - 64 = 936 is the oldest value still inside the window.
    assert device_secrets_store.accept_up_n("pgr-win-4", 936) is True
    assert device_secrets_store.accept_up_n("pgr-win-4", 935) is False


def test_accept_up_n_replay_of_a_value_that_slid_out_of_the_bitmap_is_still_caught():
    """A cold-boot epoch jump (DEVICE_PLAN.md §2.5) can shift the window by
    more than 64 in one step; the value the window slid *from* must still be
    recognised as already-accepted if it is replayed."""
    device_secrets_store.create("pgr-win-5", hmac_key=b"k" * 32, mqtt_password_hash="h")
    assert device_secrets_store.accept_up_n("pgr-win-5", 100) is True
    assert device_secrets_store.accept_up_n("pgr-win-5", 100 + (1 << 20)) is True
    # The old top (100) is now far outside the 64-wide window -- too old,
    # not a bitmap replay, but still correctly rejected either way.
    assert device_secrets_store.accept_up_n("pgr-win-5", 100) is False


def test_accept_up_n_missing_device_raises_key_error():
    with pytest.raises(KeyError):
        device_secrets_store.accept_up_n("pgr-win-missing", 1)


# ---------------------------------------------------------------------------
# next_down_n / bump_sig_failures
# ---------------------------------------------------------------------------


def test_next_down_n_increments_from_one():
    device_secrets_store.create("pgr-down-1", hmac_key=b"k" * 32, mqtt_password_hash="h")
    assert device_secrets_store.next_down_n("pgr-down-1") == 1
    assert device_secrets_store.next_down_n("pgr-down-1") == 2
    assert device_secrets_store.next_down_n("pgr-down-1") == 3
    secret = device_secrets_store.get("pgr-down-1")
    assert secret is not None
    assert secret.downN == 3


def test_next_down_n_missing_device_raises_key_error():
    with pytest.raises(KeyError):
        device_secrets_store.next_down_n("pgr-down-missing")


def test_bump_sig_failures_increments_and_returns_the_new_count():
    device_secrets_store.create("pgr-sig-1", hmac_key=b"k" * 32, mqtt_password_hash="h")
    assert device_secrets_store.bump_sig_failures("pgr-sig-1") == 1
    assert device_secrets_store.bump_sig_failures("pgr-sig-1") == 2
    secret = device_secrets_store.get("pgr-sig-1")
    assert secret is not None
    assert secret.sigFailures == 2


def test_bump_sig_failures_missing_device_raises_key_error():
    with pytest.raises(KeyError):
        device_secrets_store.bump_sig_failures("pgr-sig-missing")

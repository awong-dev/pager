"""`app/backends/sms_twilio.py` + `POST /webhooks/twilio/sms` --
docs/SERVER_PLAN.md §6.4, §5.9 ("signature checks exercised for real").

`test_compute_twilio_signature_matches_known_fixture` is the load-bearing
test: `EXPECTED_SIGNATURE` was computed *independently* of this codebase
(a standalone `hmac`/`hashlib`/`base64` script, not a call into
`sms_twilio.compute_twilio_signature`) against Twilio's own documented
algorithm (https://www.twilio.com/docs/usage/webhooks/webhooks-security),
then hardcoded here as a recorded fixture -- so a bug that breaks the
implementation's algorithm (wrong concatenation order, wrong digest, wrong
encoding) fails this test rather than passing trivially against itself.
Every other test in this file exercises the real webhook route end to end
(`TestClient` -> `verify_twilio_signature` -> `resolve_reply` ->
`Routing.send()`), not a mocked-out signature check.
"""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient

from app.backends import sms_twilio
from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient

WEBHOOK_KEY = "test-webhook-key"
AUTH_TOKEN = "test-auth-token-xyz"
PUBLIC_URL = "https://relay.example.com"
WEBHOOK_URL = f"{PUBLIC_URL}/webhooks/twilio/sms"

# Independently computed fixture -- see module docstring.
FIXTURE_PARAMS = {"To": "+15005550006", "From": "+15551234567", "Body": "hello there"}
EXPECTED_SIGNATURE = "yO/lPv1oXJR8brVrVg3Zqrl2J5w="


def test_compute_twilio_signature_matches_known_fixture():
    sig = sms_twilio.compute_twilio_signature(WEBHOOK_URL, FIXTURE_PARAMS, AUTH_TOKEN)
    assert sig == EXPECTED_SIGNATURE


def test_verify_twilio_signature_accepts_valid_and_rejects_tampering():
    assert sms_twilio.verify_twilio_signature(
        WEBHOOK_URL, FIXTURE_PARAMS, EXPECTED_SIGNATURE, AUTH_TOKEN
    )
    # Wrong token.
    assert not sms_twilio.verify_twilio_signature(
        WEBHOOK_URL, FIXTURE_PARAMS, EXPECTED_SIGNATURE, "wrong-token"
    )
    # Tampered param (a MITM/forged webhook changing the body after signing).
    tampered = {**FIXTURE_PARAMS, "Body": "hello there!"}
    assert not sms_twilio.verify_twilio_signature(
        WEBHOOK_URL, tampered, EXPECTED_SIGNATURE, AUTH_TOKEN
    )
    # Wrong URL (Twilio signs the exact URL it was configured to call).
    assert not sms_twilio.verify_twilio_signature(
        WEBHOOK_URL + "x", FIXTURE_PARAMS, EXPECTED_SIGNATURE, AUTH_TOKEN
    )
    # Missing/blank signature or token both fail closed.
    assert not sms_twilio.verify_twilio_signature(WEBHOOK_URL, FIXTURE_PARAMS, None, AUTH_TOKEN)
    assert not sms_twilio.verify_twilio_signature(WEBHOOK_URL, FIXTURE_PARAMS, EXPECTED_SIGNATURE, "")


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": WEBHOOK_KEY,
        "dev_mode": False,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture(autouse=True)
def _twilio_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("TWILIO_AUTH_TOKEN", AUTH_TOKEN)
    monkeypatch.setenv("PUBLIC_BASE_URL", PUBLIC_URL)
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)


@pytest.fixture
def client() -> Iterator[TestClient]:
    settings = make_settings()
    app = create_app(settings=settings, broker_client=FakeBrokerClient(webhook_key=WEBHOOK_KEY))
    with TestClient(app) as c:
        yield c


def _sign(params: dict[str, str]) -> str:
    return sms_twilio.compute_twilio_signature(WEBHOOK_URL, params, AUTH_TOKEN)


def _post(client: TestClient, params: dict[str, str], *, signature: str | None | object = ...):
    sig = _sign(params) if signature is ... else signature
    headers = {} if sig is None else {"X-Twilio-Signature": sig}
    return client.post("/webhooks/twilio/sms", data=params, headers=headers)


def _link_sms(uid: str, alias: str, phone: str) -> str:
    """Registers `uid` and gives them a verified `sms` backend at `phone`,
    the way `POST /api/me/backends/{id}/verify` would leave things (writes
    `phoneIndex` directly -- whitebox, matching this project's existing
    test style for store-level setup)."""
    users_store.create_user(uid=uid, alias=alias, display_name=alias)
    backend = backends_store.create_backend(uid, kind="sms", config={"phone": phone}, enabled=True)
    backends_store.update_backend(uid, backend.id, verified=True)
    backends_store.set_phone_index(phone, uid, backend.id)
    return backend.id


def test_webhook_missing_signature_is_401(client: TestClient):
    resp = _post(client, {"To": "+1", "From": "+15551234567", "Body": "hi"}, signature=None)
    assert resp.status_code == 401


def test_webhook_wrong_signature_is_401(client: TestClient):
    resp = _post(
        client, {"To": "+1", "From": "+15551234567", "Body": "hi"}, signature="not-the-real-sig"
    )
    assert resp.status_code == 401


def test_webhook_from_unlinked_number_is_dropped_with_200(client: TestClient):
    resp = _post(client, {"To": "+1", "From": "+19995551111", "Body": "hi"})
    assert resp.status_code == 200


def test_webhook_single_peer_fallback_routes_message(client: TestClient):
    users_store.create_user(uid="student", alias="student", display_name="Student")
    _link_sms("mom", "mom", "+15551234567")
    allow_store.set_edge("mom", "student", message=True, locate=True)

    resp = _post(client, {"To": "+1", "From": "+15551234567", "Body": "hi from sms"})
    assert resp.status_code == 200

    convo = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    assert len(convo) == 1
    msg = convo[0]
    assert msg.body == "hi from sms"
    assert msg.senderUid == "mom"
    assert msg.recipientUid == "student"
    assert msg.originBackendKind == "sms"
    assert msg.originBackendId is not None


def test_webhook_at_alias_prefix_routes_to_named_recipient(client: TestClient):
    users_store.create_user(uid="dad", alias="dad", display_name="Dad")
    users_store.create_user(uid="student", alias="student", display_name="Student")
    _link_sms("mom", "mom", "+15551234567")
    allow_store.set_edge("mom", "dad", message=True, locate=True)
    allow_store.set_edge("mom", "student", message=True, locate=True)

    resp = _post(client, {"To": "+1", "From": "+15551234567", "Body": "@student pick up at 5"})
    assert resp.status_code == 200

    convo = messages_store.list_thread(messages_store.conv_key("mom", "student"))
    assert len(convo) == 1
    assert convo[0].body == "pick up at 5"
    assert messages_store.list_thread(messages_store.conv_key("mom", "dad")) == []


def test_webhook_ambiguous_peer_sends_usage_hint_not_a_message(client: TestClient):
    users_store.create_user(uid="dad", alias="dad", display_name="Dad")
    users_store.create_user(uid="student", alias="student", display_name="Student")
    _link_sms("mom", "mom", "+15551234567")
    allow_store.set_edge("mom", "dad", message=True, locate=True)
    allow_store.set_edge("mom", "student", message=True, locate=True)

    resp = _post(client, {"To": "+1", "From": "+15551234567", "Body": "no alias here"})
    assert resp.status_code == 200
    assert messages_store.list_thread(messages_store.conv_key("mom", "dad")) == []
    assert messages_store.list_thread(messages_store.conv_key("mom", "student")) == []


def test_webhook_over_160_codepoints_is_rejected_not_truncated(client: TestClient):
    users_store.create_user(uid="student", alias="student", display_name="Student")
    _link_sms("mom", "mom", "+15551234567")
    allow_store.set_edge("mom", "student", message=True, locate=True)

    too_long = "x" * 161
    resp = _post(client, {"To": "+1", "From": "+15551234567", "Body": too_long})
    assert resp.status_code == 200
    # Never sent, never truncated-and-sent -- no message at all.
    assert messages_store.list_thread(messages_store.conv_key("mom", "student")) == []


def test_h1_start_link_does_not_leak_verify_code_into_config(
    monkeypatch: pytest.MonkeyPatch,
):
    """H1: the verify code (and its expiry) must never land in
    `users/{uid}/backends/{bid}.config` -- `firestore.rules` lets the
    backend's own owner read that document, i.e. exactly the person a phone
    claim needs to be verified *against*. Checked at the store level
    (`get_backend`, the same read path `firestore.rules`' `users/{uid}/
    backends/{b}` rule exposes to a client listener)."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    users_store.create_user(uid="h1", alias="h1", display_name="H1")
    backend = backends_store.create_backend(
        "h1", kind="sms", config={"phone": "+15551230000"}, enabled=False
    )
    user = users_store.get_user("h1")
    assert user is not None

    sms_twilio.SmsTwilioBackend().start_link(user, backend)

    row = backends_store.get_backend("h1", backend.id)
    assert row is not None
    assert "verifyCode" not in row.config
    assert "verifyCodeExpiresAt" not in row.config
    # Only the phone this backend was created with -- nothing verify-related.
    assert row.config == {"phone": "+15551230000"}


def test_h1_complete_link_still_works_against_the_out_of_band_stored_code(
    monkeypatch: pytest.MonkeyPatch,
):
    """The proof-check itself still functions correctly once the code moves
    out of `config` -- right code verifies, wrong code does not."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    users_store.create_user(uid="h1b", alias="h1b", display_name="H1b")
    backend = backends_store.create_backend(
        "h1b", kind="sms", config={"phone": "+15551230001"}, enabled=False
    )
    user = users_store.get_user("h1b")
    assert user is not None

    sent: list[tuple[str, str]] = []

    def fake_send_sms(to: str, body: str):
        sent.append((to, body))
        return sms_twilio.sms_client.TwilioSendResult(ok=True)

    monkeypatch.setattr(sms_twilio.sms_client, "send_sms", fake_send_sms)
    backend_impl = sms_twilio.SmsTwilioBackend()
    backend_impl.start_link(user, backend)
    import re

    match = re.search(r"\d{6}", sent[-1][1])
    assert match is not None
    code = match.group(0)

    row = backends_store.get_backend("h1b", backend.id)
    assert row is not None
    assert not backend_impl.complete_link(row, "000000" if code != "000000" else "111111")
    assert backend_impl.complete_link(row, code)


def test_m1_normalize_e164_strips_formatting_and_validates():
    assert sms_twilio.normalize_e164("+1 (555) 123-4567") == "+15551234567"
    assert sms_twilio.normalize_e164("+15551234567") == "+15551234567"
    with pytest.raises(ValueError):
        sms_twilio.normalize_e164("not a phone number")
    with pytest.raises(ValueError):
        sms_twilio.normalize_e164("+0invalid/path")


def test_webhook_self_addressed_reply_excludes_sms_origin_backend(client: TestClient):
    """S2a, exercised through the real webhook: a reply that resolves back
    to the sender's own uid (self-addressed `@alias`) must not re-queue an
    sms delivery to the backend it just arrived on, using the *real*
    `origin_backend_id` this webhook now threads through (not `None`) --
    docs/SERVER_PLAN.md §3."""
    mom_bid = _link_sms("mom", "mom", "+15551234567")
    allow_store.set_edge("mom", "mom", message=True, locate=True)

    resp = _post(client, {"To": "+1", "From": "+15551234567", "Body": "@mom note to self"})
    assert resp.status_code == 200

    convo = messages_store.list_thread(messages_store.conv_key("mom", "mom"))
    assert len(convo) == 1
    msg = convo[0]
    assert msg.originBackendId == mom_bid
    kinds = {d.kind for d in msg.deliveries.values()}
    assert "sms" not in kinds
    assert "webapp" in kinds

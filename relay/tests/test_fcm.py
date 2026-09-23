"""`app/backends/fcm.py` -- docs/V03_TASKS.md 3a.1.

`firebase_admin.messaging` itself is monkeypatched here (§5.9: "FCM sends go
through a stub `messaging` client") -- these tests never touch real FCM.
`push_tokens_store` writes/reads go through the real Firestore emulator
(the same pattern every other store test uses), so "one unregistered token
deleted, others kept" is verified against the actual store, not a mock.
"""

from __future__ import annotations

from typing import Any

import pytest
from firebase_admin import messaging

from app.backends.fcm import FirebaseFCMClient
from app.store import push_tokens as push_tokens_store


class _FakeBatchResponse:
    def __init__(self, responses: list[messaging.SendResponse]) -> None:
        self.responses = responses


def _ok(token: str) -> messaging.SendResponse:
    return messaging.SendResponse({"name": f"projects/p/messages/{token}"}, None)


def _fail(exc: Exception) -> messaging.SendResponse:
    return messaging.SendResponse(None, exc)


def _capture_multicast(monkeypatch: pytest.MonkeyPatch, responder) -> list[Any]:
    """Patches `messaging.send_each_for_multicast`, recording every
    `MulticastMessage` it was called with, and returning whatever
    `responder(message)` computes as the (fake) `BatchResponse`."""
    calls: list[Any] = []

    def _fake_send_each_for_multicast(message):
        calls.append(message)
        return responder(message)

    monkeypatch.setattr(messaging, "send_each_for_multicast", _fake_send_each_for_multicast)
    return calls


def test_send_data_success_sends_one_multicast_with_webpush_headers(
    monkeypatch: pytest.MonkeyPatch,
):
    calls = _capture_multicast(
        monkeypatch, lambda message: _FakeBatchResponse([_ok(t) for t in message.tokens])
    )

    client = FirebaseFCMClient()
    client.send_data(["tok1", "tok2"], {"kind": "message", "id": "m1"})

    assert len(calls) == 1
    message = calls[0]
    assert message.tokens == ["tok1", "tok2"]
    assert message.data == {"kind": "message", "id": "m1"}
    assert all(isinstance(v, str) for v in message.data.values())
    assert message.webpush.headers == {"Urgency": "high", "TTL": "14400"}


def test_unregistered_token_is_deleted_others_kept(monkeypatch: pytest.MonkeyPatch):
    push_tokens_store.add_token("alice", "tok_good")
    push_tokens_store.add_token("alice", "tok_dead")
    push_tokens_store.add_token("bob", "tok_other_user")

    def _responder(message: messaging.MulticastMessage) -> _FakeBatchResponse:
        responses = []
        for token in message.tokens:
            if token == "tok_dead":
                responses.append(_fail(messaging.UnregisteredError("gone")))
            else:
                responses.append(_ok(token))
        return _FakeBatchResponse(responses)

    _capture_multicast(monkeypatch, _responder)

    client = FirebaseFCMClient()
    client.send_data(["tok_good", "tok_dead", "tok_other_user"], {"id": "m1"})

    assert push_tokens_store.list_tokens("alice") == ["tok_good"]
    assert push_tokens_store.list_tokens("bob") == ["tok_other_user"]


def test_sender_id_mismatch_token_is_also_deleted(monkeypatch: pytest.MonkeyPatch):
    push_tokens_store.add_token("carol", "tok_mismatch")

    def _responder(message: messaging.MulticastMessage) -> _FakeBatchResponse:
        return _FakeBatchResponse(
            [_fail(messaging.SenderIdMismatchError("wrong sender")) for _ in message.tokens]
        )

    _capture_multicast(monkeypatch, _responder)

    client = FirebaseFCMClient()
    client.send_data(["tok_mismatch"], {"id": "m1"})

    assert push_tokens_store.list_tokens("carol") == []


def test_transient_error_keeps_the_token(
    monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
):
    push_tokens_store.add_token("dave", "tok_flaky")

    def _responder(message: messaging.MulticastMessage) -> _FakeBatchResponse:
        return _FakeBatchResponse(
            [_fail(messaging.QuotaExceededError("rate limited")) for _ in message.tokens]
        )

    _capture_multicast(monkeypatch, _responder)

    client = FirebaseFCMClient()
    with caplog.at_level("INFO", logger="relay.backends.fcm"):
        client.send_data(["tok_flaky"], {"id": "m1"})

    # Kept, not deleted: a transient/other error is not a "this token is
    # dead" signal.
    assert push_tokens_store.list_tokens("dave") == ["tok_flaky"]
    assert any("tok_flaky" in record.message for record in caplog.records)


def test_chunks_at_500_tokens(monkeypatch: pytest.MonkeyPatch):
    calls = _capture_multicast(
        monkeypatch, lambda message: _FakeBatchResponse([_ok(t) for t in message.tokens])
    )

    tokens = [f"tok{i}" for i in range(501)]
    client = FirebaseFCMClient()
    client.send_data(tokens, {"id": "m1"})

    assert len(calls) == 2
    assert len(calls[0].tokens) == 500
    assert len(calls[1].tokens) == 1
    assert calls[0].tokens + calls[1].tokens == tokens


def test_push_backend_null_constructs_the_null_client():
    from fastapi.testclient import TestClient

    from app.backends.webapp import NullFCMClient
    from app.config import Settings
    from app.main import create_app
    from tests.fake_transport import FakeBrokerClient

    settings = Settings(
        broker_api_url="http://unused.invalid/api/v5",
        broker_api_key=None,
        broker_api_secret=None,
        webhook_key="test-webhook-key",
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
        push_backend="null",
    )
    app = create_app(settings=settings, broker_client=FakeBrokerClient())
    with TestClient(app) as client:
        webapp_backend = client.app.state.backend_registry["webapp"]
        assert isinstance(webapp_backend._fcm, NullFCMClient)


def test_push_backend_fcm_constructs_the_real_client():
    from fastapi.testclient import TestClient

    from app.backends.fcm import FirebaseFCMClient
    from app.config import Settings
    from app.main import create_app
    from tests.fake_transport import FakeBrokerClient

    settings = Settings(
        broker_api_url="http://unused.invalid/api/v5",
        broker_api_key=None,
        broker_api_secret=None,
        webhook_key="test-webhook-key",
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
        push_backend="fcm",
    )
    app = create_app(settings=settings, broker_client=FakeBrokerClient())
    with TestClient(app) as client:
        webapp_backend = client.app.state.backend_registry["webapp"]
        assert isinstance(webapp_backend._fcm, FirebaseFCMClient)

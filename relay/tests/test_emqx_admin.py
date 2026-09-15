"""Unit tests for `app.emqx_admin` (docs/DEVICE_TASKS.md S2.1).

Mocks `httpx.request` -- the one function `EmqxAdmin` calls for every
authentication/authorization management call -- rather than hitting a real
EMQX, since this module's whole job is the shape of those HTTP calls, not
EMQX itself (that is what the hand-run `mosquitto_pub` proofs in the task's
Verify step are for)."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

import httpx
import pytest

from app.config import Settings
from app.emqx_admin import EmqxAdmin

BASE_URL = "http://emqx.test/api/v5"


def _settings() -> Settings:
    return Settings(
        broker_api_url=BASE_URL,
        broker_api_key="test-key",
        broker_api_secret="test-secret",
        webhook_key="wk",
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
    )


@dataclass
class _FakeResponse:
    status_code: int
    text: str = ""


@dataclass
class _RequestLog:
    calls: list[tuple[str, str, dict[str, Any] | None]] = field(default_factory=list)


def _install_fake_httpx(
    monkeypatch: pytest.MonkeyPatch, responder
) -> _RequestLog:
    log = _RequestLog()

    def fake_request(method: str, url: str, *, json=None, auth=None, timeout=None):
        log.calls.append((method, url, json))
        assert auth == ("test-key", "test-secret")
        return responder(method, url, json)

    monkeypatch.setattr(httpx, "request", fake_request)
    return log


def test_ensure_device_creates_user_and_pushes_acl(monkeypatch: pytest.MonkeyPatch) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        if method == "POST" and url.endswith("/users"):
            return _FakeResponse(201)
        if method == "PUT" and "/authorization/" in url:
            return _FakeResponse(204)
        raise AssertionError(f"unexpected call: {method} {url}")

    log = _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    result = admin.ensure_device("pgr-0001", "s3cret", "pgr-0001")

    assert result == "ok"
    assert len(log.calls) == 2
    create_method, create_url, create_body = log.calls[0]
    assert create_method == "POST"
    assert create_url == f"{BASE_URL}/authentication/password_based:built_in_database/users"
    assert create_body == {"user_id": "pgr-0001", "password": "s3cret", "is_superuser": False}

    acl_method, acl_url, acl_body = log.calls[1]
    assert acl_method == "PUT"
    assert (
        acl_url
        == f"{BASE_URL}/authorization/sources/built_in_database/rules/users/pgr-0001"
    )
    assert acl_body is not None
    assert acl_body["username"] == "pgr-0001"
    rules = acl_body["rules"]
    assert {"topic": "pager/pgr-0001/up", "permission": "allow", "action": "publish"} in rules
    assert {"topic": "pager/pgr-0001/status", "permission": "allow", "action": "publish"} in rules
    assert {"topic": "pager/pgr-0001/loc", "permission": "allow", "action": "publish"} in rules
    assert {"topic": "pager/pgr-0001/down", "permission": "allow", "action": "subscribe"} in rules
    assert len(rules) == 4


def test_ensure_device_falls_back_to_put_on_conflict(monkeypatch: pytest.MonkeyPatch) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        if method == "POST" and url.endswith("/users"):
            return _FakeResponse(409, "already exists")
        if method == "PUT" and "/authentication/" in url:
            assert url.endswith("/users/pgr-0002")
            return _FakeResponse(200)
        if method == "PUT" and "/authorization/" in url:
            return _FakeResponse(204)
        raise AssertionError(f"unexpected call: {method} {url}")

    log = _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    result = admin.ensure_device("pgr-0002", "newpw", "pgr-0002")

    assert result == "ok"
    assert len(log.calls) == 3


def test_ensure_device_reports_error_on_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        return _FakeResponse(500, "boom")

    _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    assert admin.ensure_device("pgr-0003", "pw", "pgr-0003") == "error"


def test_ensure_boot_user_uses_boot_prefixed_username_and_topics(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        if method == "POST" and url.endswith("/users"):
            return _FakeResponse(201)
        if method == "PUT" and "/authorization/" in url:
            return _FakeResponse(204)
        raise AssertionError(f"unexpected call: {method} {url}")

    log = _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    result = admin.ensure_boot_user("abc123def456", "bpw")

    assert result == "ok"
    _create_method, _create_url, create_body = log.calls[0]
    assert create_body == {"user_id": "boot-abc123def456", "password": "bpw", "is_superuser": False}

    _acl_method, acl_url, acl_body = log.calls[1]
    assert acl_url.endswith("/users/boot-abc123def456")
    assert acl_body is not None
    rules = acl_body["rules"]
    assert {
        "topic": "pager/boot/abc123def456/up",
        "permission": "allow",
        "action": "publish",
    } in rules
    assert {
        "topic": "pager/boot/abc123def456/down",
        "permission": "allow",
        "action": "subscribe",
    } in rules
    assert len(rules) == 2


def test_delete_user_removes_authn_and_authz(monkeypatch: pytest.MonkeyPatch) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        assert method == "DELETE"
        return _FakeResponse(204)

    log = _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    result = admin.delete_user("pgr-0001")

    assert result == "ok"
    assert len(log.calls) == 2
    urls = {url for _, url, _ in log.calls}
    assert f"{BASE_URL}/authentication/password_based:built_in_database/users/pgr-0001" in urls
    assert (
        f"{BASE_URL}/authorization/sources/built_in_database/rules/users/pgr-0001" in urls
    )


def test_delete_user_treats_404_as_success(monkeypatch: pytest.MonkeyPatch) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        return _FakeResponse(404, "not found")

    _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    assert admin.delete_user("ghost") == "ok"


def test_delete_user_reports_error_on_unexpected_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def responder(method: str, url: str, json: dict[str, Any] | None) -> _FakeResponse:
        return _FakeResponse(500, "boom")

    _install_fake_httpx(monkeypatch, responder)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    assert admin.delete_user("pgr-0001") == "error"


def test_network_failure_is_reported_as_error_never_raises(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fake_request(method, url, *, json=None, auth=None, timeout=None):
        raise httpx.ConnectError("connection refused")

    monkeypatch.setattr(httpx, "request", fake_request)
    admin = EmqxAdmin(_settings(), manages_auth=True)

    assert admin.ensure_device("pgr-0001", "pw", "pgr-0001") == "error"
    assert admin.delete_user("pgr-0001") == "error"


def test_broker_manages_auth_false_is_a_noop(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_request(*args, **kwargs):
        raise AssertionError("httpx.request must not be called when BROKER_MANAGES_AUTH=0")

    monkeypatch.setattr(httpx, "request", fake_request)
    admin = EmqxAdmin(_settings(), manages_auth=False)

    assert admin.ensure_device("pgr-0001", "pw", "pgr-0001") == "manual"
    assert admin.ensure_boot_user("bid123", "bpw") == "manual"
    assert admin.delete_user("pgr-0001") == "manual"


def test_default_manages_auth_reads_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("BROKER_MANAGES_AUTH", "0")

    def fake_request(*args, **kwargs):
        raise AssertionError("httpx.request must not be called when BROKER_MANAGES_AUTH=0")

    monkeypatch.setattr(httpx, "request", fake_request)
    admin = EmqxAdmin(_settings())

    assert admin.ensure_device("pgr-0001", "pw", "pgr-0001") == "manual"

    monkeypatch.setenv("BROKER_MANAGES_AUTH", "1")
    admin2 = EmqxAdmin(_settings())
    assert admin2._manages_auth is True

"""Unit tests for app.wire envelope validation, independent of MQTT."""

from __future__ import annotations

import pytest
from pydantic import ValidationError

from app.wire import (
    StatusEnvelope,
    UpEnvelope,
    parse_envelope_bytes,
    resolve_ts,
    strip_control_chars,
)


def test_ack_envelope_valid():
    env = UpEnvelope.model_validate({"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "ack": "shown"})
    assert env.is_ack
    assert env.body is None


def test_content_envelope_valid():
    env = UpEnvelope.model_validate(
        {"v": 1, "id": "u_aaaaaaaa", "ts": 1_700_000_000, "from": "student", "body": "ok", "ack": None}
    )
    assert not env.is_ack
    assert env.body == "ok"


def test_ack_with_body_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "ack": "shown", "body": "nope"}
        )


def test_content_message_without_body_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {"v": 1, "id": "u_aaaaaaaa", "ts": 1_700_000_000, "from": "student", "ack": None}
        )


def test_content_message_body_over_160_codepoints_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {
                "v": 1,
                "id": "u_aaaaaaaa",
                "ts": 1_700_000_000,
                "from": "student",
                "body": "a" * 161,
                "ack": None,
            }
        )


def test_content_message_body_with_control_char_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {
                "v": 1,
                "id": "u_aaaaaaaa",
                "ts": 1_700_000_000,
                "from": "student",
                "body": "hi\x01there",
                "ack": None,
            }
        )


def test_invalid_id_format_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate({"v": 1, "id": "BAD ID!", "ts": 1_700_000_000, "ack": "shown"})


def test_unknown_fields_are_ignored():
    env = UpEnvelope.model_validate(
        {
            "v": 1,
            "id": "m_aaaaaaaa",
            "ts": 1_700_000_000,
            "ack": "shown",
            "prio": "high",  # reserved future field, §11
        }
    )
    assert env.is_ack


def test_status_online_requires_mode_batt_ts():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate({"v": 1, "state": "online", "session": "s_aabbccdd"})


def test_status_offline_only_needs_session():
    env = StatusEnvelope.model_validate({"v": 1, "state": "offline", "session": "s_aabbccdd"})
    assert env.state == "offline"


def test_status_invalid_session_format_is_malformed():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate({"v": 1, "state": "offline", "session": "not-a-session"})


def test_parse_envelope_bytes_rejects_oversize():
    huge = b'{"body":"' + b"x" * 700 + b'"}'
    assert parse_envelope_bytes(huge) is None


def test_parse_envelope_bytes_rejects_invalid_utf8():
    assert parse_envelope_bytes(b"\xff\xfe") is None


def test_parse_envelope_bytes_rejects_non_object():
    assert parse_envelope_bytes(b"[1,2,3]") is None
    assert parse_envelope_bytes(b'"just a string"') is None


def test_parse_envelope_bytes_accepts_valid_object():
    assert parse_envelope_bytes(b'{"a":1}') == {"a": 1}


def test_resolve_ts_substitutes_for_zero():
    assert resolve_ts(1_700_000_000) == 1_700_000_000
    assert resolve_ts(0) > 0


def test_strip_control_chars():
    assert strip_control_chars("a\x00b\x1fc\x7fd") == "abcd"

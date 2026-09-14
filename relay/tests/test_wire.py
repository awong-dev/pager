"""Unit tests for app.wire envelope validation, independent of MQTT."""

from __future__ import annotations

import json

import pytest
from pydantic import ValidationError

from app.wire import (
    DownEnvelope,
    LocEnvelope,
    StatusEnvelope,
    UpEnvelope,
    build_down_payload,
    is_valid_alias,
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


def test_ack_with_to_is_malformed():
    """S3: §3.1 restricts `to` to up content messages; an ack carrying `to`
    is malformed."""
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "ack": "shown", "to": "mom"}
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


# ---- §3.1 alias shape (from/to) ----


@pytest.mark.parametrize(
    "alias",
    [
        "parent",
        "student",
        "mom",
        "a",  # 1 char, minimum length
        "a" * 16,  # 16 chars, maximum length
        "user-1",
        "user_1",
        "u1_-a",
        "0start",  # may start with a digit
    ],
)
def test_valid_alias_shapes(alias: str):
    assert is_valid_alias(alias)


@pytest.mark.parametrize(
    "alias",
    [
        "a" * 17,  # too long
        "Parent",  # uppercase
        "-leading-hyphen",
        "_leading-underscore",
        "",  # empty
        "has space",
        "hasüni",
    ],
)
def test_invalid_alias_shapes(alias: str):
    assert not is_valid_alias(alias)


def test_from_system_is_a_valid_alias():
    assert is_valid_alias("system")
    env = UpEnvelope.model_validate(
        {
            "v": 1,
            "id": "u_aaaaaaaa",
            "ts": 1_700_000_000,
            "from": "system",
            "body": "hi",
            "ack": None,
        }
    )
    assert env.from_ == "system"


def test_up_from_invalid_alias_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {
                "v": 1,
                "id": "u_aaaaaaaa",
                "ts": 1_700_000_000,
                "from": "Not Valid",
                "body": "hi",
                "ack": None,
            }
        )


# ---- §3.1/§3.2 `to` (up messages) ----


def test_up_message_with_to_present():
    env = UpEnvelope.model_validate(
        {
            "v": 1,
            "id": "u_aaaaaaaa",
            "ts": 1_700_000_000,
            "from": "student",
            "to": "mom",
            "body": "ok coming",
            "ack": None,
        }
    )
    assert env.to == "mom"


def test_up_message_with_to_absent():
    env = UpEnvelope.model_validate(
        {
            "v": 1,
            "id": "u_aaaaaaaa",
            "ts": 1_700_000_000,
            "from": "student",
            "body": "ok coming",
            "ack": None,
        }
    )
    assert env.to is None


def test_up_message_with_invalid_to_is_malformed():
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {
                "v": 1,
                "id": "u_aaaaaaaa",
                "ts": 1_700_000_000,
                "from": "student",
                "to": "Bad To!",
                "body": "ok coming",
                "ack": None,
            }
        )


def test_up_worst_case_up_message_fits_640_byte_hard_limit():
    """§3.3: a maximal up message carrying `to` -- from, to and body all at
    their maximum size -- is documented as a 604-byte arithmetic ceiling
    (built from a conservative per-field estimate) and must in any case
    stay under the 640-byte hard limit. The concretely achievable worst
    case, built here with a body at both the 160-codepoint and 320-UTF-8-
    byte caps simultaneously (160 two-byte, non-ASCII code points -- the
    same total as 160 escaped quote characters would give), minifies to
    438 bytes with `ensure_ascii=False` (real devices emit raw UTF-8, not
    `\\uXXXX` escapes) -- comfortably under both figures."""
    body = "ñ" * 160  # 160 code points, 320 UTF-8 bytes: the
    # heaviest legal body shape allowed by validate_body.
    obj = {
        "v": 1,
        "id": "u" * 16,
        "ts": 1_700_000_000,
        "from": "a" * 16,
        "to": "b" * 16,
        "body": body,
        "ack": None,
    }
    raw = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    assert len(raw) <= 640
    assert len(raw) > 400  # sanity: this really is a near-worst-case payload

    data = parse_envelope_bytes(raw)
    assert data is not None
    env = UpEnvelope.model_validate(data)
    assert env.from_ == "a" * 16
    assert env.to == "b" * 16
    assert env.body == body


# ---- §3.1/§3.2 down envelope `kind` ----


def test_down_envelope_default_kind_is_msg():
    env = DownEnvelope.model_validate(
        {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "from": "parent", "body": "hi", "ack": None}
    )
    assert env.kind == "msg"


def test_down_envelope_loc_req_has_no_body():
    env = DownEnvelope.model_validate(
        {
            "v": 1,
            "id": "m_aaaaaaaa",
            "ts": 1_700_000_000,
            "kind": "loc_req",
            "from": "mom",
            "ack": None,
        }
    )
    assert env.kind == "loc_req"
    assert env.body is None


def test_down_envelope_loc_req_with_body_is_malformed():
    with pytest.raises(ValidationError):
        DownEnvelope.model_validate(
            {
                "v": 1,
                "id": "m_aaaaaaaa",
                "ts": 1_700_000_000,
                "kind": "loc_req",
                "from": "mom",
                "body": "should not be here",
                "ack": None,
            }
        )


def test_down_envelope_msg_without_body_is_malformed():
    with pytest.raises(ValidationError):
        DownEnvelope.model_validate(
            {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "from": "parent", "ack": None}
        )


def test_build_down_payload_default_kind_omits_kind_key():
    raw = build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000, body="hi")
    obj = json.loads(raw)
    assert "kind" not in obj
    assert obj["body"] == "hi"
    assert obj["ack"] is None


def test_build_down_payload_loc_req_has_no_body_key():
    raw = build_down_payload(
        msg_id="m_aaaaaaaa", ts=1_700_000_000, kind="loc_req", from_="mom"
    )
    obj = json.loads(raw)
    assert obj["kind"] == "loc_req"
    assert "body" not in obj
    assert obj["ack"] is None
    assert len(raw) <= 640
    # Round-trips through the same shape validation used elsewhere.
    DownEnvelope.model_validate(obj)


# ---- S1: build_down_payload must never produce a malformed msg envelope ----


def test_build_down_payload_msg_without_body_raises():
    with pytest.raises(ValueError):
        build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000)


def test_build_down_payload_msg_with_empty_body_raises():
    with pytest.raises(ValueError):
        build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000, body="")


@pytest.mark.parametrize("kind", ["msg", "loc_req"])
def test_build_down_payload_round_trips_or_raises(kind: str):
    """For each `kind`, either build_down_payload raises (no body given for
    a `msg`), or the payload it produces round-trips cleanly through
    DownEnvelope -- there is no way to get a payload out of the builder that
    DownEnvelope itself would reject."""
    kwargs: dict[str, object] = {"msg_id": "m_aaaaaaaa", "ts": 1_700_000_000, "kind": kind}
    if kind == "msg":
        kwargs["body"] = "hello"
    raw = build_down_payload(**kwargs)
    env = DownEnvelope.model_validate(json.loads(raw))
    assert env.kind == kind


def test_build_down_payload_msg_no_body_raises_before_producing_bad_json():
    """The specific S1 regression: omitting body for kind="msg" must raise,
    not silently emit `"body":""`, which DownEnvelope (and the device) would
    reject as malformed."""
    with pytest.raises(ValueError):
        build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000, kind="msg")


# ---- §13.2 LocEnvelope ----


def _base_loc(**overrides):
    obj = {
        "v": 1,
        "id": "l_3c9a11f0",
        "ts": 1_700_000_000,
        "loc": {
            "lat": 37.774929,
            "lon": -122.419416,
            "acc": 14,
            "fix_ts": 1_700_000_000,
            "src": "gnss",
        },
        "req": None,
        "cached": False,
    }
    obj.update(overrides)
    return obj


def test_loc_envelope_valid_periodic_fix():
    env = LocEnvelope.model_validate(_base_loc())
    assert env.req is None
    assert env.err is None
    assert env.loc is not None
    assert env.loc.lat == pytest.approx(37.774929)
    assert env.cached is False


def test_loc_envelope_valid_on_demand_answer_with_req():
    env = LocEnvelope.model_validate(_base_loc(req="m_7f3a2b10", cached=True))
    assert env.req == "m_7f3a2b10"
    assert env.cached is True


def test_loc_envelope_valid_no_fix_error_shape():
    env = LocEnvelope.model_validate(
        {
            "v": 1,
            "id": "l_3c9a11f0",
            "ts": 1_700_000_000,
            "loc": None,
            "req": "m_7f3a2b10",
            "err": "no_fix",
        }
    )
    assert env.loc is None
    assert env.err == "no_fix"


def test_loc_envelope_valid_disabled_error_shape():
    env = LocEnvelope.model_validate(
        {
            "v": 1,
            "id": "l_3c9a11f0",
            "ts": 1_700_000_000,
            "loc": None,
            "req": None,
            "err": "disabled",
        }
    )
    assert env.err == "disabled"


def test_loc_envelope_both_loc_and_err_set_is_malformed():
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(_base_loc(err="no_fix"))


def test_loc_envelope_neither_loc_nor_err_set_is_malformed():
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(
            {
                "v": 1,
                "id": "l_3c9a11f0",
                "ts": 1_700_000_000,
                "loc": None,
                "req": None,
            }
        )


def test_loc_envelope_oversize_payload_rejected():
    huge = json.dumps(_base_loc()).encode("utf-8") + b" " * 700
    assert parse_envelope_bytes(huge) is None


def test_loc_envelope_missing_req_key_is_rejected():
    """§13.2: `req` is a required key (`id | null`) -- omitting it entirely
    (as opposed to sending `req: null`) must be rejected."""
    obj = _base_loc()
    del obj["req"]
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(obj)


@pytest.mark.parametrize(
    "overrides",
    [
        {"lat": 90.0001},
        {"lat": -90.0001},
    ],
)
def test_loc_fix_lat_out_of_range_is_malformed(overrides: dict):
    obj = _base_loc()
    obj["loc"].update(overrides)
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(obj)


@pytest.mark.parametrize(
    "overrides",
    [
        {"lon": 180.0001},
        {"lon": -180.0001},
    ],
)
def test_loc_fix_lon_out_of_range_is_malformed(overrides: dict):
    obj = _base_loc()
    obj["loc"].update(overrides)
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(obj)


def test_loc_fix_negative_acc_is_malformed():
    obj = _base_loc()
    obj["loc"]["acc"] = -1
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(obj)


def test_loc_fix_zero_fix_ts_with_real_loc_is_malformed():
    """§3.5's ts=0 substitution convention is for the envelope's top-level
    `ts`, not `loc.fix_ts`: a real fix with fix_ts=0 is nonsense."""
    obj = _base_loc()
    obj["loc"]["fix_ts"] = 0
    with pytest.raises(ValidationError):
        LocEnvelope.model_validate(obj)


def test_loc_fix_valid_boundary_values_accepted():
    obj = _base_loc()
    obj["loc"].update({"lat": 90.0, "lon": -180.0, "acc": 0})
    env = LocEnvelope.model_validate(obj)
    assert env.loc is not None
    assert env.loc.lat == 90.0
    assert env.loc.lon == -180.0
    assert env.loc.acc == 0


def test_loc_envelope_example_from_protocol_round_trips():
    raw = (
        b'{"v":1,"id":"l_3c9a11f0","ts":1757700000,'
        b'"loc":{"lat":37.774929,"lon":-122.419416,"acc":14,'
        b'"fix_ts":1757699991,"src":"gnss"},'
        b'"req":"m_7f3a2b10","cached":false}'
    )
    assert len(raw) <= 640
    data = parse_envelope_bytes(raw)
    assert data is not None
    env = LocEnvelope.model_validate(data)
    assert env.req == "m_7f3a2b10"
    assert env.loc is not None
    assert env.loc.src == "gnss"

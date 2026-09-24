"""Unit tests for app.wire envelope validation, independent of MQTT."""

from __future__ import annotations

import json

import pytest
from pydantic import ValidationError

from app import devauth, wirecbor
from app.wire import (
    CellInfo,
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


# ---------------------------------------------------------------------------
# docs/V02_DESIGN.md §3: `n` widens from a 32-bit to a 52-bit counter
# (< 2**53). §14.2's `n = (epoch << 20) | lo` with a 32-bit epoch means a
# real device can now report an `n` well above the old 2**32 ceiling.
# ---------------------------------------------------------------------------


def _online_status(**overrides: object) -> dict:
    base = {
        "v": 1,
        "state": "online",
        "mode": "sleep",
        "batt_mv": 3280,
        "rssi": -85,
        "session": "s_aabbccdd",
        "ts": 1_700_000_000,
    }
    base.update(overrides)
    return base


@pytest.mark.parametrize("n", [0, 2**32 - 1, 2**32, 2**32 + 1, 2**53 - 1])
def test_n_up_to_2_53_minus_1_is_accepted_on_every_signed_kind(n):
    assert UpEnvelope.model_validate(
        {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "ack": "shown", "n": n}
    ).n == n
    assert StatusEnvelope.model_validate(_online_status(n=n)).n == n
    assert (
        LocEnvelope.model_validate(
            {
                "v": 1,
                "id": "l_aaaaaaaa",
                "ts": 1_700_000_000,
                "loc": None,
                "req": None,
                "err": "no_fix",
                "n": n,
            }
        ).n
        == n
    )


@pytest.mark.parametrize("n", [2**53, 2**53 + 1, -1])
def test_n_at_or_above_2_53_or_negative_is_rejected(n):
    with pytest.raises(ValidationError):
        UpEnvelope.model_validate(
            {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "ack": "shown", "n": n}
        )
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(n=n))


# ---------------------------------------------------------------------------
# docs/V02_DESIGN.md §4.3/§5/§7 -- new optional `/status` fields. Ground
# rule §0: "unknown or new optional /status fields must never cause
# rejection", and these four are the concrete new fields that must be
# *accepted* (not merely ignored) before any firmware sends them.
# ---------------------------------------------------------------------------


def test_status_accepts_tls_ca_fp_loc_backoff_s_sms_lost():
    env = StatusEnvelope.model_validate(
        _online_status(tls="broken", ca_fp="0123456789abcdef", loc_backoff_s=320, sms_lost=2)
    )
    assert env.tls == "broken"
    assert env.ca_fp == "0123456789abcdef"
    assert env.loc_backoff_s == 320
    assert env.sms_lost == 2


def test_status_tls_ca_fp_loc_backoff_s_sms_lost_are_optional():
    env = StatusEnvelope.model_validate(_online_status())
    assert env.tls is None
    assert env.ca_fp is None
    assert env.loc_backoff_s is None
    assert env.sms_lost is None


def test_status_rejects_bad_tls_value():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(tls="not-a-state"))


def test_status_rejects_malformed_ca_fp():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(ca_fp="too-short"))
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(ca_fp="0123456789ABCDEF"))  # must be lowercase


def test_status_rejects_negative_sms_lost():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(sms_lost=-1))


# ---------------------------------------------------------------------------
# docs/V02_DESIGN.md §9.5/§7 (this task) -- `/status`'s optional `link`
# field: MQTT-session generation within a boot.
# ---------------------------------------------------------------------------


def test_status_accepts_link():
    env = StatusEnvelope.model_validate(_online_status(link=3))
    assert env.link == 3


def test_status_link_is_optional():
    """Absent-field compatibility: older firmware that never sends `link`
    must still validate, with `env.link` reported as unknown (`None`), not
    `0`."""
    env = StatusEnvelope.model_validate(_online_status())
    assert env.link is None


def test_status_rejects_negative_link():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(link=-1))


def test_status_accepts_link_zero():
    env = StatusEnvelope.model_validate(_online_status(link=0))
    assert env.link == 0


# ---------------------------------------------------------------------------
# docs/WIFI_DESIGN.md §6/§7, docs/WIFI_TASKS.md W7 -- `/status`'s optional
# `xport` field: which physical transport carried this session.
# ---------------------------------------------------------------------------


def test_status_accepts_xport_wifi_and_lte():
    assert StatusEnvelope.model_validate(_online_status(xport="wifi")).xport == "wifi"
    assert StatusEnvelope.model_validate(_online_status(xport="lte")).xport == "lte"


def test_status_xport_is_optional():
    """Older firmware that predates the WiFi transport never sends `xport`;
    such a `/status` must still validate, with `env.xport` reported as
    unknown (`None`), same compatibility rule `link` gets above."""
    env = StatusEnvelope.model_validate(_online_status())
    assert env.xport is None


def test_status_rejects_bad_xport_value():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(xport="modem"))


# ---------------------------------------------------------------------------
# Crash diagnostics (this task, docs/PROTOCOL.md §5.1) -- `/status`'s
# optional `rst`/`stage`/`abn` fields.
# ---------------------------------------------------------------------------


def test_status_accepts_rst_stage_abn():
    env = StatusEnvelope.model_validate(_online_status(rst=4, stage=6, abn=2))
    assert env.rst == 4
    assert env.stage == 6
    assert env.abn == 2


def test_status_rst_stage_abn_are_optional():
    """Absent-field compatibility: older firmware that predates these
    fields must still validate, reported as unknown (`None`)."""
    env = StatusEnvelope.model_validate(_online_status())
    assert env.rst is None
    assert env.stage is None
    assert env.abn is None


@pytest.mark.parametrize("field", ["rst", "stage"])
def test_status_rejects_rst_stage_out_of_range(field):
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(**{field: -1}))
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(**{field: 256}))


def test_status_accepts_rst_stage_boundary_values():
    assert StatusEnvelope.model_validate(_online_status(rst=0)).rst == 0
    assert StatusEnvelope.model_validate(_online_status(rst=255)).rst == 255
    assert StatusEnvelope.model_validate(_online_status(stage=0)).stage == 0
    assert StatusEnvelope.model_validate(_online_status(stage=255)).stage == 255


def test_status_rejects_abn_out_of_range():
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(abn=-1))
    with pytest.raises(ValidationError):
        StatusEnvelope.model_validate(_online_status(abn=65536))


def test_status_accepts_abn_boundary_values():
    assert StatusEnvelope.model_validate(_online_status(abn=0)).abn == 0
    assert StatusEnvelope.model_validate(_online_status(abn=65535)).abn == 65535


def test_status_rst_stage_abn_round_trip_cbor():
    """docs/PROTOCOL.md §10: keys 53/54/55 -- next free integers after the
    already-shipped `xport=52` (see wirecbor.py's own discrepancy note)."""
    obj = _online_status(rst=4, stage=6, abn=2)
    assert wirecbor.KEYMAP["rst"] == 53
    assert wirecbor.KEYMAP["stage"] == 54
    assert wirecbor.KEYMAP["abn"] == 55
    cbor_bytes = wirecbor.encode(obj)
    decoded = wirecbor.decode(cbor_bytes)
    assert decoded["rst"] == 4
    assert decoded["stage"] == 6
    assert decoded["abn"] == 2
    env = StatusEnvelope.model_validate(decoded)
    assert (env.rst, env.stage, env.abn) == (4, 6, 2)


def test_status_still_ignores_a_genuinely_unknown_field():
    """The pre-existing `extra='ignore'` forward-compat guarantee, still
    true for a field this relay has no opinion on at all (as opposed to the
    four modelled-and-validated fields above)."""
    env = StatusEnvelope.model_validate(_online_status(some_future_field="whatever"))
    assert env.state == "online"


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


# ---- §3.1/§4 `sndr` (group chat, docs/GROUP_CHAT_DESIGN.md), task G3 ----


def test_down_envelope_sndr_absent_is_valid_and_none():
    """A DM page never carries `sndr` -- absent parses cleanly to `None`,
    exactly today's shape."""
    env = DownEnvelope.model_validate(
        {"v": 1, "id": "m_aaaaaaaa", "ts": 1_700_000_000, "from": "mom", "body": "hi", "ack": None}
    )
    assert env.sndr is None


def test_down_envelope_sndr_present_on_group_msg():
    env = DownEnvelope.model_validate(
        {
            "v": 1,
            "id": "m_aaaaaaaa",
            "ts": 1_700_000_000,
            "from": "family",
            "sndr": "mom",
            "body": "hi",
            "ack": None,
        }
    )
    assert env.sndr == "mom"
    assert env.from_ == "family"


def test_down_envelope_invalid_sndr_alias_is_malformed():
    with pytest.raises(ValidationError):
        DownEnvelope.model_validate(
            {
                "v": 1,
                "id": "m_aaaaaaaa",
                "ts": 1_700_000_000,
                "from": "family",
                "sndr": "Bad Alias!",
                "body": "hi",
                "ack": None,
            }
        )


def test_down_envelope_sndr_on_loc_req_is_malformed():
    """§4: `sndr` only ever appears on a `msg` -- group `/locate` is out of
    scope, so a `loc_req` carrying it is malformed."""
    with pytest.raises(ValidationError):
        DownEnvelope.model_validate(
            {
                "v": 1,
                "id": "m_aaaaaaaa",
                "ts": 1_700_000_000,
                "kind": "loc_req",
                "from": "mom",
                "sndr": "mom",
                "ack": None,
            }
        )


def test_build_down_payload_omits_sndr_by_default():
    raw = build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000, body="hi", from_="mom")
    assert "sndr" not in json.loads(raw)


def test_build_down_payload_dm_bytes_byte_identical_regardless_of_sndr_support():
    """Pins the exact bytes of an ordinary (DM) down payload -- adding
    `sndr` support must not change a single byte of a page that doesn't use
    it."""
    raw = build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000, body="hi", from_="mom")
    assert raw == b'{"v":1,"id":"m_aaaaaaaa","ts":1700000000,"from":"mom","body":"hi","ack":null}'


def test_build_down_payload_with_sndr_round_trips_and_orders_after_from():
    raw = build_down_payload(
        msg_id="m_aaaaaaaa", ts=1_700_000_000, body="hi", from_="family", sndr="mom"
    )
    obj = json.loads(raw)
    assert obj["sndr"] == "mom"
    # Field order: from, then sndr, then body (docs/PROTOCOL.md's amended
    # "SHOULD emit" order: sndr takes the slot `to` would occupy).
    keys = list(obj.keys())
    assert keys.index("from") < keys.index("sndr") < keys.index("body")
    DownEnvelope.model_validate(obj)


def test_build_down_payload_sndr_on_loc_req_raises():
    with pytest.raises(ValueError):
        build_down_payload(msg_id="m_aaaaaaaa", ts=1_700_000_000, kind="loc_req", sndr="mom")


def test_group_down_page_carries_sndr_in_both_encodings():
    raw = build_down_payload(
        msg_id="m_aaaaaaaa", ts=1_700_000_000, body="hi", from_="family", sndr="mom"
    )
    obj = json.loads(raw)
    assert obj["sndr"] == "mom"

    cbor_bytes = wirecbor.encode(obj)
    assert wirecbor.KEYMAP["sndr"] == 51
    decoded = wirecbor.decode(cbor_bytes)
    assert decoded["sndr"] == "mom"
    assert decoded["from"] == "family"


def test_down_worst_case_group_message_signed_fits_640_bytes_both_encodings():
    """docs/GROUP_CHAT_DESIGN.md §3.3: the worst-case signed group `/down
    msg` -- 16-char id, 16-char `from` (the group alias), 16-char `sndr`
    (the author's alias), body at 160 code points / 320 UTF-8 bytes -- fits
    under the 640-byte hard limit in both encodings, matching §3.3's own
    ~505-byte (signed JSON) figure. Uses the real `app.devauth` signer (the
    same one `app/broker.py`'s `publish_down` calls), so this is the actual
    production wire size, not an estimate."""
    body = "ñ" * 160  # 160 code points, 320 UTF-8 bytes -- validate_body's
    # heaviest legal body shape, same as test_up_worst_case_up_message_...
    obj = {
        "v": 1,
        "id": "m" * 16,
        "ts": 1_700_000_000,
        "from": "g" * 16,
        "sndr": "s" * 16,
        "body": body,
        "n": 2**53 - 1,  # worst-case 52-bit replay counter (§14.2)
        "ack": None,
    }
    key = b"k" * 32
    topic = "pager/pgr-0001/down"

    json_signed = devauth.sign_json(key, topic, obj)
    assert len(json_signed) <= 640
    assert len(json_signed) > 400  # sanity: a near-worst-case payload

    cbor_signed = devauth.sign_cbor(key, topic, obj)
    assert len(cbor_signed) <= 640

    # The unsigned shape (sans `n`, which DownEnvelope doesn't model -- it's
    # bolted on at sign time, same as every other down envelope) is a valid
    # group `msg`.
    DownEnvelope.model_validate({k: v for k, v in obj.items() if k != "n"})


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


# ---- §13.2 `cell` (cell-tower location fallback, this task) ----


def _cell(**overrides) -> dict:
    obj = {"mcc": "310", "mnc": "410", "tac": 12345, "ci": 87654321, "rsrp": -95}
    obj.update(overrides)
    return obj


def test_cell_info_valid_full():
    info = CellInfo.model_validate(_cell())
    assert info.mcc == "310"
    assert info.mnc == "410"
    assert info.tac == 12345
    assert info.ci == 87654321
    assert info.rsrp == -95


def test_cell_info_valid_without_rsrp():
    info = CellInfo.model_validate(_cell(rsrp=None))
    assert info.rsrp is None


def test_cell_info_valid_two_digit_mnc_leading_zero_preserved():
    info = CellInfo.model_validate(_cell(mnc="05"))
    assert info.mnc == "05"


def test_loc_envelope_no_fix_with_valid_cell():
    """§13.2: "the pager sends `cell` whenever it answers without a GNSS
    fix": `loc: null, err: "no_fix", cell: {...}`."""
    obj = {
        "v": 1,
        "id": "l_3c9a11f0",
        "ts": 1_700_000_000,
        "loc": None,
        "req": "m_7f3a2b10",
        "err": "no_fix",
        "cell": _cell(),
    }
    env = LocEnvelope.model_validate(obj)
    assert env.loc is None
    assert env.err == "no_fix"
    assert env.cell is not None
    assert env.cell.mcc == "310"
    assert env.cell.ci == 87654321


def test_loc_envelope_cell_absent_is_backward_compatible():
    """A pager that never sends `cell` (today's firmware) must behave
    exactly as today: `env.cell` is `None`, nothing else about the envelope
    changes."""
    env = LocEnvelope.model_validate(_base_loc())
    assert env.cell is None


def test_loc_envelope_gnss_fix_wins_cell_is_only_recorded():
    """§13.2: "It may also send it alongside a real GNSS fix (then the GNSS
    fix wins and the cell is only recorded)." -- both a real `loc` and a
    `cell` validate together; nothing about `LocEnvelope` itself picks a
    winner (that's `app/location.py`'s job) -- this only pins that the wire
    shape accepts both at once."""
    obj = _base_loc()
    obj["cell"] = _cell()
    env = LocEnvelope.model_validate(obj)
    assert env.loc is not None
    assert env.cell is not None


@pytest.mark.parametrize(
    "bad_cell",
    [
        {"mcc": "31", "mnc": "410", "tac": 1, "ci": 1},  # mcc not 3 digits
        {"mcc": "310", "mnc": "4100", "tac": 1, "ci": 1},  # mnc too long
        {"mcc": "31x", "mnc": "410", "tac": 1, "ci": 1},  # mcc non-digit
        {"mcc": "310", "mnc": "410", "tac": -1, "ci": 1},  # tac out of range
        {"mcc": "310", "mnc": "410", "tac": 65536, "ci": 1},  # tac out of range
        {"mcc": "310", "mnc": "410", "tac": 1, "ci": -1},  # ci out of range
        {"mcc": "310", "mnc": "410", "tac": 1, "ci": 268_435_456},  # ci out of range (2**28)
        {"mcc": "310", "mnc": "410", "tac": 1, "ci": 1, "rsrp": -29},  # rsrp out of range
        {"mcc": "310", "mnc": "410", "tac": 1, "ci": 1, "rsrp": -157},  # rsrp out of range
        {"mcc": "310", "mnc": "410"},  # missing required tac/ci
        "not-a-map",
    ],
)
def test_loc_envelope_malformed_cell_is_treated_as_absent(bad_cell):
    """§13.2 (this task): "treat a malformed cell as absent rather than
    dropping the envelope" -- the rest of the `/loc` envelope (a valid
    `no_fix` answer here) must still parse successfully."""
    obj = {
        "v": 1,
        "id": "l_3c9a11f0",
        "ts": 1_700_000_000,
        "loc": None,
        "req": None,
        "err": "no_fix",
        "cell": bad_cell,
    }
    env = LocEnvelope.model_validate(obj)
    assert env.cell is None
    assert env.err == "no_fix"


def test_loc_envelope_cell_null_is_absent():
    obj = _base_loc()
    obj["cell"] = None
    env = LocEnvelope.model_validate(obj)
    assert env.cell is None

"""`app/logging_config.py` -- punch
list item 6 (JSON logs with a `severity` field for Cloud Logging, plus a
per-request correlation id). The correlation-id *middleware* itself is
exercised end to end by `tests/test_healthz.py`'s `X-Request-Id` tests;
this file covers the formatter in isolation."""

from __future__ import annotations

import json
import logging

from app.logging_config import JsonLogFormatter, request_id_var


def _make_record(**kwargs) -> logging.LogRecord:
    defaults = {
        "name": "relay.test",
        "level": logging.INFO,
        "pathname": __file__,
        "lineno": 1,
        "msg": "hello %s",
        "args": ("world",),
        "exc_info": None,
    }
    defaults.update(kwargs)
    return logging.LogRecord(**defaults)


def test_format_produces_valid_json_with_severity_message_timestamp():
    record = _make_record()
    line = JsonLogFormatter().format(record)
    payload = json.loads(line)
    assert payload["severity"] == "INFO"
    assert payload["message"] == "hello world"
    assert "timestamp" in payload
    assert payload["logger"] == "relay.test"


def test_format_maps_every_standard_level_to_its_cloud_logging_severity():
    expected = {
        logging.DEBUG: "DEBUG",
        logging.INFO: "INFO",
        logging.WARNING: "WARNING",
        logging.ERROR: "ERROR",
        logging.CRITICAL: "CRITICAL",
    }
    for level, severity in expected.items():
        record = _make_record(level=level)
        payload = json.loads(JsonLogFormatter().format(record))
        assert payload["severity"] == severity


def test_format_includes_request_id_when_set_in_context():
    token = request_id_var.set("req-abc-123")
    try:
        record = _make_record()
        payload = json.loads(JsonLogFormatter().format(record))
        assert payload["requestId"] == "req-abc-123"
    finally:
        request_id_var.reset(token)


def test_format_omits_request_id_when_not_set():
    record = _make_record()
    payload = json.loads(JsonLogFormatter().format(record))
    assert "requestId" not in payload


def test_format_includes_exception_traceback_when_present():
    try:
        raise ValueError("boom")
    except ValueError:
        import sys

        record = _make_record(msg="failed", args=(), exc_info=sys.exc_info())
    payload = json.loads(JsonLogFormatter().format(record))
    assert "ValueError" in payload["exception"]
    assert "boom" in payload["exception"]

"""tools/soracom_beam.py request builder (pure; the tool itself is not run live in tests)."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "soracom_beam", Path(__file__).resolve().parents[2] / "tools" / "soracom_beam.py"
)
assert _spec and _spec.loader
sb = importlib.util.module_from_spec(_spec)
sys.modules["soracom_beam"] = sb
_spec.loader.exec_module(sb)


def test_build_calls_without_group_id():
    calls = sb.build_calls("pager-beam", "295050000000001", sb.DEFAULT_DESTINATION)
    assert [(c.method, c.path) for c in calls] == [
        ("GET", "/groups"),
        ("POST", "/groups"),
        ("PUT", "/groups/{group_id}/configuration/SoracomBeam"),
        ("POST", "/subscribers/295050000000001/set_group"),
        ("GET", "/groups/{group_id}"),
    ]
    assert calls[1].json == {"tags": {"name": "pager-beam"}}
    value = calls[2].json[0]["value"]
    assert calls[2].json[0]["key"] == "mqtt://beam.soracom.io:1883"
    assert value["useClientCredentials"] is True and value["version"] == "201912"
    assert value["destination"].startswith("mqtts://s1289801.ala.us-east-1.emqxsl.com:8883")


def test_build_calls_with_known_group_id_skips_lookup():
    calls = sb.build_calls("g", "1", "mqtts://h:8883", group_id="grp-1")
    assert [(c.method, c.path) for c in calls] == [
        ("PUT", "/groups/grp-1/configuration/SoracomBeam"),
        ("POST", "/subscribers/1/set_group"),
        ("GET", "/groups/grp-1"),
    ]
    assert calls[1].json == {"groupId": "grp-1"}


def test_dry_run_redacts_secrets(monkeypatch, capsys):
    monkeypatch.setenv("SORACOM_AUTH_KEY_ID", "keyId-SECRETID")
    monkeypatch.setenv("SORACOM_AUTH_KEY", "secret-SECRETKEY")
    assert sb.main(["--imsi", "1", "--dry-run"]) == 0
    out = capsys.readouterr().out
    assert "SECRET" not in out and "<redacted>" in out
    assert "POST https://g.api.soracom.io/v1/auth" in out

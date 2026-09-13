#!/usr/bin/env python3
"""
End-to-end integration test for the school pager relay (HANDOFF.md Phase 6).

Brings up the *real* docker-compose stack (`relay/docker-compose.yml`: an
eclipse-mosquitto broker + the relay container -- exactly what a developer
runs locally with `docker compose up`) and exercises scenarios the relay's
unit tests cannot reach, because `relay/tests/` talks to an in-process fake
MQTT transport (`relay/tests/fake_transport.py`), never a real socket:

  1. a full round trip over a real paho-mqtt wire connection, covering all
     four ack states (queued/sent/shown/read) plus a student reply,
  2. garbage bytes published directly to the real broker on `/up` and
     `/status` by a client that goes around the relay's own code entirely,
  3. the broker process disappearing and coming back while the relay keeps
     running,
  4. a message queued for a device that has never connected ("power-on with
     no network coverage yet").

Requires: Docker with the Compose plugin, and `paho-mqtt` importable by the
interpreter running this script -- the same dependency `relay/pyproject.toml`
and `tools/sim_device.py` already require. Easiest: run with
`relay/.venv/bin/python3` if it exists, otherwise `pip install paho-mqtt`
into whatever interpreter you use to run this file; `tools/sim_device.py` is
invoked as a subprocess with `sys.executable`, so the same interpreter must
have paho-mqtt too.

Usage:
    python3 tools/e2e_test.py

Exit code 0 if every scenario passes, 1 otherwise. Always tears down the
compose stack (`docker compose down -v`) and restores/removes any
`relay/.env` this script wrote, whether scenarios pass or fail. Never leaves
containers running or a real-looking `.env` behind.
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from queue import Empty, Queue
from typing import Any

import paho.mqtt.client as mqtt

logger = logging.getLogger("e2e_test")

REPO_ROOT = Path(__file__).resolve().parent.parent
RELAY_DIR = REPO_ROOT / "relay"
COMPOSE_FILE = RELAY_DIR / "docker-compose.yml"
ENV_PATH = RELAY_DIR / ".env"
SIM_DEVICE_SCRIPT = REPO_ROOT / "tools" / "sim_device.py"

RELAY_URL = "http://localhost:8000"
MQTT_HOST = "localhost"
MQTT_PORT = 1883

_env_backup: str | None = None
_scratch_dir: Path | None = None
_sim_logs: list[tuple[str, Path]] = []


# --------------------------------------------------------------------------
# Small generic helpers
# --------------------------------------------------------------------------


def wait_until(
    predicate: Callable[[], bool],
    *,
    timeout: float,
    interval: float = 0.5,
    description: str = "condition",
) -> None:
    """Poll `predicate` until it returns True or `timeout` seconds elapse.

    Raises AssertionError with a description and the last error seen (if
    `predicate` raised) rather than letting a raw exception or a silent
    `False` propagate -- every e2e failure should say what it was waiting
    for.
    """
    deadline = time.monotonic() + timeout
    last_exc: Exception | None = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except Exception as exc:  # noqa: BLE001 - collected for the failure message
            last_exc = exc
        time.sleep(interval)
    extra = f" (last error while polling: {last_exc!r})" if last_exc else ""
    raise AssertionError(
        f"timed out after {timeout}s waiting for: {description}{extra}"
    )


def api_get(path: str, token: str) -> tuple[int, Any]:
    req = urllib.request.Request(
        f"{RELAY_URL}{path}", headers={"Authorization": f"Bearer {token}"}
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8") or "null")
    except urllib.error.HTTPError as exc:
        body = exc.read()
        try:
            return exc.code, json.loads(body.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return exc.code, body


def api_post(path: str, token: str, payload: dict) -> tuple[int, Any]:
    req = urllib.request.Request(
        f"{RELAY_URL}{path}",
        data=json.dumps(payload).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {token}",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8") or "null")
    except urllib.error.HTTPError as exc:
        body = exc.read()
        try:
            return exc.code, json.loads(body.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return exc.code, body


def get_thread(device_id: str, token: str) -> list[dict]:
    status, data = api_get(f"/api/devices/{device_id}/messages", token)
    assert status == 200, f"GET messages for {device_id} failed: {status} {data}"
    assert isinstance(data, list)
    return data


def find_message(device_id: str, msg_id: str, token: str) -> dict | None:
    for m in get_thread(device_id, token):
        if m["id"] == msg_id:
            return m
    return None


def send_message(device_id: str, token: str, body: str) -> dict:
    status, data = api_post(f"/api/devices/{device_id}/messages", token, {"body": body})
    assert status == 200, f"POST message to {device_id} failed: {status} {data}"
    return data


# --------------------------------------------------------------------------
# docker compose plumbing
# --------------------------------------------------------------------------


def compose(
    *args: str, check: bool = True, capture: bool = False
) -> subprocess.CompletedProcess:
    cmd = ["docker", "compose", "-f", str(COMPOSE_FILE), *args]
    print(f"$ {' '.join(cmd)}")
    return subprocess.run(
        cmd,
        cwd=str(RELAY_DIR),
        check=check,
        capture_output=capture,
        text=True,
        timeout=180,
    )


def get_container_id(service: str) -> str:
    result = compose("ps", "-q", service, capture=True)
    return result.stdout.strip()


def write_env_file(token: str) -> None:
    global _env_backup
    if ENV_PATH.exists():
        _env_backup = ENV_PATH.read_text()
    ENV_PATH.write_text(
        "\n".join(
            [
                f"RELAY_TOKEN={token}",
                "MQTT_BROKER_HOST=broker",
                "MQTT_BROKER_PORT=1883",
                "MQTT_USERNAME=",
                "MQTT_PASSWORD=",
                "",
            ]
        )
    )


def restore_env_file() -> None:
    if _env_backup is not None:
        ENV_PATH.write_text(_env_backup)
    else:
        ENV_PATH.unlink(missing_ok=True)


def _probe_relay_http(token: str) -> bool:
    try:
        status, _ = api_get("/api/devices/e2e-startup-probe/status", token)
        return status in (200, 404)
    except (urllib.error.URLError, OSError, TimeoutError):
        return False


def start_fresh_stack(token: str, *, build: bool) -> None:
    """Bring up a brand-new broker + relay pair for one scenario. Each
    scenario gets its own stack rather than sharing one for the whole run:
    a bug that leaves the relay's MQTT connection permanently dead (see
    scenario 3) must not cascade into false failures in later, unrelated
    scenarios."""
    args = ["up", "-d", "--build"] if build else ["up", "-d"]
    compose(*args)
    wait_until(
        lambda: _probe_relay_http(token),
        timeout=60,
        interval=1,
        description="relay HTTP API to come up",
    )


def dump_logs() -> None:
    for service, tail in (("relay", "80"), ("broker", "30")):
        print(f"\n--- docker compose logs {service} --tail {tail} ---")
        result = compose("logs", service, "--tail", tail, check=False, capture=True)
        print(result.stdout)
    for device_id, path in _sim_logs:
        print(f"\n--- sim_device.py log for {device_id} ({path}) ---")
        try:
            print(path.read_text())
        except OSError as exc:
            print(f"(could not read log: {exc})")


def stop_stack() -> None:
    print("\n=== tearing down this scenario's docker compose stack ===")
    compose("down", "-v", check=False)


def final_cleanup() -> None:
    restore_env_file()
    if _scratch_dir is not None:
        shutil.rmtree(_scratch_dir, ignore_errors=True)


# --------------------------------------------------------------------------
# Simulated device subprocess (tools/sim_device.py)
# --------------------------------------------------------------------------


def start_sim_device(
    device_id: str, extra_args: list[str] | None = None
) -> subprocess.Popen:
    assert _scratch_dir is not None
    log_path = _scratch_dir / f"sim_{device_id}_{os.urandom(3).hex()}.log"
    log_file = log_path.open("w")
    cmd = [
        sys.executable,
        "-u",
        str(SIM_DEVICE_SCRIPT),
        "--device-id",
        device_id,
        "--host",
        MQTT_HOST,
        "--port",
        str(MQTT_PORT),
    ]
    if extra_args:
        cmd.extend(extra_args)
    print(f"$ {' '.join(cmd)}  (log: {log_path})")
    proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT, text=True)
    proc._e2e_log_file = log_file  # type: ignore[attr-defined]
    _sim_logs.append((device_id, log_path))
    return proc


def stop_sim_device(proc: subprocess.Popen | None) -> None:
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    log_file = getattr(proc, "_e2e_log_file", None)
    if log_file is not None:
        log_file.close()


# --------------------------------------------------------------------------
# Inline paho-mqtt device: full manual control (used by scenario 1, where we
# need to drive shown *and* read acks plus a reply in a specific order --
# more than tools/sim_device.py's reference behaviour (auto-ack shown only)
# supports).
# --------------------------------------------------------------------------


class InlineDevice:
    def __init__(self, device_id: str) -> None:
        self.device_id = device_id
        self.session_id = "s_" + os.urandom(4).hex()
        self.down_topic = f"pager/{device_id}/down"
        self.up_topic = f"pager/{device_id}/up"
        self.status_topic = f"pager/{device_id}/status"

        self._incoming: Queue[tuple[str, bytes]] = Queue()
        self._connected = threading.Event()
        self._subscribed = threading.Event()

        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=device_id,
            clean_session=False,
            protocol=mqtt.MQTTv311,
        )
        self._client.will_set(
            self.status_topic,
            json.dumps(
                {"v": 1, "state": "offline", "session": self.session_id},
                separators=(",", ":"),
            ),
            qos=1,
            retain=True,
        )
        self._client.on_connect = self._on_connect
        self._client.on_subscribe = self._on_subscribe
        self._client.on_message = self._on_message

    def connect(self, *, timeout: float = 10.0) -> None:
        self._client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
        self._client.loop_start()
        if not self._connected.wait(timeout=timeout):
            raise AssertionError(
                f"inline device {self.device_id} failed to connect to broker"
            )
        if not self._subscribed.wait(timeout=timeout):
            raise AssertionError(
                f"inline device {self.device_id} failed to subscribe to /down"
            )

    def close(self) -> None:
        self._client.loop_stop()
        try:
            self._client.disconnect()
        except Exception:
            logger.debug(
                "disconnect for inline device %s raised, ignoring",
                self.device_id,
                exc_info=True,
            )

    def _on_connect(
        self, client, userdata, connect_flags, reason_code, properties
    ) -> None:
        client.subscribe(self.down_topic, qos=1)
        self._publish_online_status()
        self._connected.set()

    def _on_subscribe(
        self, client, userdata, mid, reason_code_list, properties
    ) -> None:
        self._subscribed.set()

    def _on_message(self, client, userdata, msg) -> None:
        self._incoming.put((msg.topic, msg.payload))

    def _publish_online_status(self) -> None:
        payload = json.dumps(
            {
                "v": 1,
                "state": "online",
                "mode": "sleep",
                "batt_mv": 3280,
                "rssi": -85,
                "session": self.session_id,
                "ts": int(time.time()),
                "fw": "e2e-inline-0.1.0",
            },
            separators=(",", ":"),
        )
        self._client.publish(self.status_topic, payload, qos=1, retain=True)

    def wait_for_down_message(self, timeout: float = 15.0) -> dict:
        try:
            topic, payload = self._incoming.get(timeout=timeout)
        except Empty as exc:
            raise AssertionError(
                f"timed out after {timeout}s waiting for a /down message on {self.down_topic}"
            ) from exc
        assert topic == self.down_topic, f"unexpected topic {topic}"
        return json.loads(payload.decode("utf-8"))

    def ack(self, msg_id: str, ack: str) -> None:
        payload = json.dumps(
            {"v": 1, "id": msg_id, "ts": int(time.time()), "ack": ack},
            separators=(",", ":"),
        )
        self._client.publish(self.up_topic, payload, qos=1)

    def reply(self, body: str) -> str:
        reply_id = "u_" + os.urandom(4).hex()
        payload = json.dumps(
            {
                "v": 1,
                "id": reply_id,
                "ts": int(time.time()),
                "from": "student",
                "body": body,
                "ack": None,
            },
            separators=(",", ":"),
        )
        self._client.publish(self.up_topic, payload, qos=1)
        return reply_id


# --------------------------------------------------------------------------
# Scenario 1: full round trip, all four ack states + a reply
# --------------------------------------------------------------------------


def scenario_1_full_roundtrip(token: str) -> None:
    device_id = "e2e-roundtrip-1"
    dev = InlineDevice(device_id)
    dev.connect()
    try:
        sent = send_message(device_id, token, "Pickup at 3:15 by the gym")
        msg_id = sent["id"]
        assert sent["state"] == "queued", (
            f"expected queued right after POST, got {sent}"
        )

        down = dev.wait_for_down_message(timeout=15)
        assert down["id"] == msg_id, f"device received wrong id: {down}"
        assert down["body"] == "Pickup at 3:15 by the gym"
        assert down["ack"] is None

        wait_until(
            lambda: (
                (m := find_message(device_id, msg_id, token)) is not None
                and m["state"] == "sent"
            ),
            timeout=10,
            description=f"message {msg_id} to reach state=sent (broker PUBACK)",
        )

        dev.ack(msg_id, "shown")
        wait_until(
            lambda: (
                (m := find_message(device_id, msg_id, token)) is not None
                and m["state"] == "shown"
            ),
            timeout=10,
            description=f"message {msg_id} to reach state=shown",
        )

        dev.ack(msg_id, "read")
        wait_until(
            lambda: (
                (m := find_message(device_id, msg_id, token)) is not None
                and m["state"] == "read"
            ),
            timeout=10,
            description=f"message {msg_id} to reach state=read",
        )

        reply_id = dev.reply("ok coming")
        wait_until(
            lambda: (
                (m := find_message(device_id, reply_id, token)) is not None
                and m["direction"] == "up"
                and m["body"] == "ok coming"
            ),
            timeout=10,
            description=f"student reply {reply_id} to appear in the thread",
        )
    finally:
        dev.close()


# --------------------------------------------------------------------------
# Scenario 2: malformed payloads over the real broker
# --------------------------------------------------------------------------

_PAD = "A" * 700  # pushes any payload well over the 640-byte hard limit (§3.3)

MALFORMED_UP_PAYLOADS: list[tuple[str, bytes]] = [
    ("invalid-json", b'{"id": "m_bad0001", this is not json'),
    ("missing-required-fields", json.dumps({"foo": "bar"}).encode("utf-8")),
    (
        "oversized-640-bytes",
        json.dumps(
            {
                "v": 1,
                "id": "m_bad0002",
                "ts": 1_700_000_000,
                "ack": "shown",
                "pad": _PAD,
            }
        ).encode("utf-8"),
    ),
    (
        "ack-and-body-both-set",
        json.dumps(
            {
                "v": 1,
                "id": "m_bad0003",
                "ts": 1_700_000_000,
                "ack": "shown",
                "from": "parent",
                "body": "should never be allowed with ack set",
            }
        ).encode("utf-8"),
    ),
]

MALFORMED_STATUS_PAYLOADS: list[tuple[str, bytes]] = [
    ("invalid-json", b'{"state": "online" this is not json'),
    ("missing-required-fields", json.dumps({"foo": "bar"}).encode("utf-8")),
    (
        "oversized-640-bytes",
        json.dumps(
            {
                "v": 1,
                "state": "online",
                "mode": "sleep",
                "batt_mv": 3300,
                "session": "s_deadbeef",
                "ts": 1_700_000_000,
                "pad": _PAD,
            }
        ).encode("utf-8"),
    ),
]


def scenario_2_malformed(token: str) -> None:
    device_id = "e2e-malformed-1"
    raw = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="e2e-raw-publisher",
    )
    raw.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    raw.loop_start()
    try:
        for label, payload in MALFORMED_UP_PAYLOADS:
            raw.publish(f"pager/{device_id}/up", payload, qos=1)
            time.sleep(1.0)
            status, data = api_get(f"/api/devices/{device_id}/messages", token)
            assert status == 200, (
                f"relay HTTP API stopped responding after malformed /up payload "
                f"'{label}' ({status}: {data})"
            )
            assert data == [], (
                f"malformed /up payload '{label}' created spurious message row(s): {data}"
            )

        for label, payload in MALFORMED_STATUS_PAYLOADS:
            raw.publish(f"pager/{device_id}/status", payload, qos=1)
            time.sleep(1.0)
            status, data = api_get(f"/api/devices/{device_id}/status", token)
            assert status == 404, (
                f"malformed /status payload '{label}' should not create a status row "
                f"(expected 404, got {status}: {data})"
            )
            status, data = api_get(f"/api/devices/{device_id}/messages", token)
            assert status == 200, (
                f"relay HTTP API stopped responding after malformed /status payload "
                f"'{label}' ({status}: {data})"
            )
    finally:
        raw.loop_stop()
        raw.disconnect()

    # Full liveness check: after all that garbage, the relay must still be
    # able to do a completely ordinary send and get a broker PUBACK for it.
    liveness_device = "e2e-malformed-liveness"
    sent = send_message(liveness_device, token, "still alive after garbage")
    wait_until(
        lambda: (
            (m := find_message(liveness_device, sent["id"], token)) is not None
            and m["state"] == "sent"
        ),
        timeout=10,
        description="relay to still publish and get a PUBACK after malformed-payload storm",
    )


# --------------------------------------------------------------------------
# Scenario 3: broker outage recovery
# --------------------------------------------------------------------------


def scenario_3_broker_outage(token: str) -> None:
    device_id = "e2e-outage-1"
    sim = start_sim_device(device_id)
    try:
        wait_until(
            lambda: api_get(f"/api/devices/{device_id}/status", token)[0] == 200,
            timeout=15,
            description="simulated device to report itself online before the outage",
        )

        relay_cid_before = get_container_id("relay")
        assert relay_cid_before, "could not determine the relay container id"

        # "a message already in flight": sent while broker+device are both up.
        send_message(device_id, token, "in flight before the outage")

        compose("stop", "broker")
        time.sleep(2)

        status, data = api_get(f"/api/devices/{device_id}/messages", token)
        assert status == 200, (
            f"relay HTTP API stopped responding while the broker was down "
            f"({status}: {data})"
        )

        # The relay must accept new sends even though it cannot currently
        # reach the broker -- the message should sit at 'queued'.
        sent_during_outage = send_message(device_id, token, "queued during the outage")
        msg_id_b = sent_during_outage["id"]
        assert sent_during_outage["state"] == "queued", (
            f"expected a message sent while the broker is down to be queued, "
            f"got {sent_during_outage}"
        )

        compose("start", "broker")

        try:
            wait_until(
                lambda: (
                    (m := find_message(device_id, msg_id_b, token)) is not None
                    and m["state"] in ("sent", "shown", "read")
                ),
                timeout=45,
                interval=2,
                description=(
                    f"relay to reconnect to the broker on its own and publish "
                    f"message {msg_id_b} (queued during the outage) -- no relay "
                    f"restart is expected or permitted here"
                ),
            )
        except AssertionError as exc:
            logs = compose(
                "logs", "relay", "--tail", "200", check=False, capture=True
            ).stdout
            hint = ""
            if "Exception in thread paho-mqtt-client" in logs:
                hint = (
                    " NOTE: the relay's MQTT background thread crashed with an "
                    "unhandled exception on disconnect (see relay container logs) "
                    "and therefore never attempted to reconnect. This looks like a "
                    "real bug in app/mqtt_transport.py, not a test-timing issue."
                )
            raise AssertionError(f"{exc}.{hint}") from exc

        relay_cid_after = get_container_id("relay")
        assert relay_cid_before == relay_cid_after, (
            "the relay container was restarted during the outage test "
            f"(before={relay_cid_before!r} after={relay_cid_after!r}); the relay "
            "is supposed to recover on its own without a restart"
        )

        # A newly sent message must reach the (re-)connected simulated device.
        sent_after = send_message(device_id, token, "post-recovery delivery check")
        msg_id_c = sent_after["id"]
        wait_until(
            lambda: (
                (m := find_message(device_id, msg_id_c, token)) is not None
                and m["state"] == "shown"
            ),
            timeout=30,
            interval=1,
            description=(
                f"message {msg_id_c}, sent after recovery, to reach state=shown on "
                "the reconnected simulated device"
            ),
        )
    finally:
        stop_sim_device(sim)
        compose("start", "broker", check=False)


# --------------------------------------------------------------------------
# Scenario 4: power-on with no network / device that has never connected
# --------------------------------------------------------------------------


def scenario_4_poweron_no_network(token: str) -> None:
    device_id = "e2e-poweron-1"

    sent = send_message(device_id, token, "power-on queued test")
    msg_id = sent["id"]
    assert sent["state"] == "queued", (
        f"expected a message to a never-connected device to be queued, got {sent}"
    )

    status, _ = api_get(f"/api/devices/{device_id}/status", token)
    assert status == 404, (
        f"expected 404 for a device that has never connected, got {status}"
    )

    sim = start_sim_device(device_id)
    try:
        wait_until(
            lambda: (
                (m := find_message(device_id, msg_id, token)) is not None
                and m["state"] == "shown"
            ),
            timeout=20,
            interval=1,
            description=(
                f"message {msg_id} (queued before the device ever connected) to be "
                "delivered and acked shown once the device powers on"
            ),
        )
    finally:
        stop_sim_device(sim)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

SCENARIOS: list[tuple[str, Callable[[str], None]]] = [
    (
        "full round-trip: queued -> sent -> shown -> read + student reply",
        scenario_1_full_roundtrip,
    ),
    ("malformed payloads over the real broker", scenario_2_malformed),
    ("broker outage recovery", scenario_3_broker_outage),
    (
        "power-on with no network (message queued before device ever connects)",
        scenario_4_poweron_no_network,
    ),
]


def main() -> None:
    global _scratch_dir
    _scratch_dir = Path(tempfile.mkdtemp(prefix="pager-e2e-"))

    token = "e2e_" + os.urandom(12).hex()
    results: list[tuple[str, bool, str]] = []

    try:
        write_env_file(token)
        for i, (name, fn) in enumerate(SCENARIOS):
            print(f"\n=== SCENARIO: {name} ===")
            _sim_logs.clear()
            try:
                # Build the image once (first scenario); reuse it after that
                # -- each scenario still gets its own fresh containers.
                start_fresh_stack(token, build=(i == 0))
                fn(token)
            except Exception as exc:  # noqa: BLE001 - reported, not swallowed
                print(f"FAIL: {name}\n  reason: {exc}")
                dump_logs()
                results.append((name, False, str(exc)))
            else:
                print(f"PASS: {name}")
                results.append((name, True, ""))
            finally:
                stop_stack()
    finally:
        final_cleanup()

    print("\n=== SUMMARY ===")
    for name, ok, reason in results:
        line = f"[{'PASS' if ok else 'FAIL'}] {name}"
        if reason:
            line += f"\n       {reason}"
        print(line)

    all_ok = len(results) == len(SCENARIOS) and all(ok for _, ok, _ in results)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()

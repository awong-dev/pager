#!/usr/bin/env python3
"""
End-to-end integration suite for the v2 pager relay -- docs/SERVER_PLAN.md
§8. Brings up the real `relay/docker-compose.yml` stack (EMQX + Firebase
emulators + relay, `DEV_MODE=1`), imports `tools/pager_client.py` **as a
library** (one process plays the pager device *and* every human in the
scenario), and runs the named scenarios below. This phase (3) implements
scenarios 1-4; 5-9 (location, fan-out, retention, byte accounting) are later
phases per `HANDOFF_V2.md` §5.

Usage:
    python3 tools/e2e_v2.py                 # all of scenarios 1-4
    python3 tools/e2e_v2.py bootstrap allowlist

Exit code 0 if every requested scenario passes, 1 otherwise. Always tears
down the compose stack it started.

**What this script found about EMQX device-credential support (Phase 3
build note, see the final report):** `tools/emqx_setup.py` and
`relay/docker-compose.yml` provision only (a) the rule-engine
connector/action/rule that forwards `pager/+/{up,status,loc}` to the
relay's webhook, and (b) the relay's own dashboard/REST API key (the
bootstrap file `relay/emqx/bootstrap_api_keys.txt`). Neither configures
EMQX's `authentication`/`authorization` chain at all, so the local EMQX
container accepts **anonymous** MQTT connections and enforces no
topic ACLs -- `POST /api/admin/devices` mints and returns a per-device MQTT
username/password (and this script exercises that call, per scenario 1),
but nothing pushes that credential (or the three ACL rules PROTOCOL.md §2
describes) into EMQX, and the device in this suite connects without
presenting them at all. This matches `docs/SERVER_PLAN.md` §5.5's own
hedge ("If the broker's REST API exposes credential and ACL management ...
otherwise the admin UI shows 'add this to the broker' copy") -- it is not a
regression introduced here, just an unbuilt piece of Phase 2a/3, recorded
per this phase's brief rather than silently assumed.
"""

from __future__ import annotations

import argparse
import logging
import os
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

logger = logging.getLogger("e2e_v2")

REPO_ROOT = Path(__file__).resolve().parent.parent
RELAY_DIR = REPO_ROOT / "relay"
TOOLS_DIR = REPO_ROOT / "tools"
COMPOSE_FILE = RELAY_DIR / "docker-compose.yml"
ENV_PATH = RELAY_DIR / ".env"
EMQX_SETUP_SCRIPT = TOOLS_DIR / "emqx_setup.py"
RELAY_VENV_PYTHON = RELAY_DIR / ".venv" / "bin" / "python3"

RELAY_URL = "http://localhost:8000"
AUTH_URL = "http://localhost:9099"
MQTT_HOST = "localhost"
MQTT_PORT = 1883
EMQX_API_URL = "http://localhost:18083"
FIREBASE_PROJECT_ID = "demo-pager"
BROKER_API_KEY = "dev-broker-api-key"
BROKER_API_SECRET = "dev-broker-api-secret"
WEBHOOK_KEY = "dev-webhook-key"

sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(RELAY_DIR))
import httpx
import pager_client

_env_backup: str | None = None


# --------------------------------------------------------------------------
# compose plumbing (same pattern as tools/e2e_test.py)
# --------------------------------------------------------------------------


def compose(*args: str, check: bool = True, capture: bool = False) -> subprocess.CompletedProcess:
    cmd = ["docker", "compose", "-f", str(COMPOSE_FILE), *args]
    print(f"$ {' '.join(cmd)}")
    return subprocess.run(
        cmd, cwd=str(RELAY_DIR), check=check, capture_output=capture, text=True, timeout=180
    )


def wait_until(
    predicate: Callable[[], bool], *, timeout: float, interval: float = 0.5, description: str = ""
) -> None:
    deadline = time.monotonic() + timeout
    last_exc: Exception | None = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except Exception as exc:  # noqa: BLE001 -- collected for the failure message
            last_exc = exc
        time.sleep(interval)
    extra = f" (last error: {last_exc!r})" if last_exc else ""
    raise AssertionError(f"timed out after {timeout}s waiting for: {description}{extra}")


def write_env_file() -> None:
    global _env_backup
    if ENV_PATH.exists():
        _env_backup = ENV_PATH.read_text()
    ENV_PATH.write_text(
        "\n".join(
            [
                "RELAY_TOKEN=unused-in-v2",
                "BROKER_API_URL=http://emqx:18083/api/v5",
                f"BROKER_API_KEY={BROKER_API_KEY}",
                f"BROKER_API_SECRET={BROKER_API_SECRET}",
                f"WEBHOOK_KEY={WEBHOOK_KEY}",
                "",
            ]
        )
    )


def restore_env_file() -> None:
    if _env_backup is not None:
        ENV_PATH.write_text(_env_backup)
    else:
        ENV_PATH.unlink(missing_ok=True)


def configure_emqx() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(EMQX_SETUP_SCRIPT),
            "--emqx-url",
            EMQX_API_URL,
            "--relay-url",
            "http://relay:8000",
            "--webhook-key",
            WEBHOOK_KEY,
        ],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    print(result.stdout, end="")
    if result.returncode != 0:
        print(result.stderr, end="", file=sys.stderr)
        raise AssertionError(f"tools/emqx_setup.py failed (exit {result.returncode})")


def _relay_reachable() -> bool:
    try:
        resp = httpx.get(f"{RELAY_URL}/healthz", timeout=3.0)
        return resp.status_code == 200
    except httpx.HTTPError:
        return False


def _emqx_reachable() -> bool:
    try:
        resp = httpx.get(f"{EMQX_API_URL}/api/v5/status", timeout=3.0)
        return resp.status_code == 200
    except httpx.HTTPError:
        return False


def _auth_emulator_reachable() -> bool:
    # The compose healthcheck only probes Firestore's port (8080), and
    # `/healthz` only round-trips Firestore too -- neither guarantees the
    # Auth emulator (9099, in the same `firebase` container) has finished
    # starting. `bootstrap_admin`/`pager_client.ServerClient.login` both hit
    # 9099 directly, so wait for it explicitly rather than relying on
    # `_relay_reachable` as a proxy for "the whole `firebase` container is
    # ready" (observed empirically: a `ConnectionResetError` from 9099 while
    # 8080 already answers, i.e. exactly this race).
    try:
        # Same path `relay/tests/conftest.py` DELETEs to wipe state between
        # tests (only DELETE is a valid method on it, hence 405, not 404/5xx)
        # -- any HTTP response at all means the emulator's HTTP server is up
        # and routing; a connection error/reset means it is not.
        resp = httpx.get(
            f"{AUTH_URL}/emulator/v1/projects/{FIREBASE_PROJECT_ID}/accounts", timeout=3.0
        )
        return resp.status_code == 405
    except httpx.HTTPError:
        return False


def start_stack(*, build: bool) -> None:
    write_env_file()
    compose(*(["up", "-d", "--build"] if build else ["up", "-d"]))
    wait_until(_relay_reachable, timeout=120, interval=1, description="relay to become healthy")
    wait_until(
        _auth_emulator_reachable, timeout=60, interval=1, description="Auth emulator to become reachable"
    )
    configure_emqx()


def stop_stack() -> None:
    print("\n=== tearing down the compose stack ===")
    compose("down", "-v", check=False)
    restore_env_file()


def dump_logs() -> None:
    for service, tail in (("relay", "120"), ("emqx", "60"), ("firebase", "40")):
        print(f"\n--- docker compose logs {service} --tail {tail} ---")
        result = compose("logs", service, "--tail", tail, check=False, capture=True)
        print(result.stdout)


def bootstrap_admin(alias: str = "admin", email: str = "admin@example.com") -> None:
    """Runs `python -m app.bootstrap` from the host against the compose
    stack's published emulator ports (8080/9099) -- the same emulators the
    `relay` container talks to over the compose network, just reached from
    the other side of the port mapping."""
    python = str(RELAY_VENV_PYTHON) if RELAY_VENV_PYTHON.exists() else sys.executable
    env = dict(os.environ)
    env.update(
        FIRESTORE_EMULATOR_HOST="localhost:8080",
        FIREBASE_AUTH_EMULATOR_HOST="localhost:9099",
        GOOGLE_CLOUD_PROJECT=FIREBASE_PROJECT_ID,
    )
    result = subprocess.run(
        [python, "-m", "app.bootstrap", "--admin-email", email, "--alias", alias],
        cwd=str(RELAY_DIR),
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    print(result.stdout, end="")
    if result.returncode != 0:
        print(result.stderr, end="", file=sys.stderr)
        raise AssertionError("app.bootstrap failed")


def _use_relay_store() -> None:
    """Sets the emulator env vars `app.db.firestore` reads (must happen
    before its first use anywhere in this process, same rule
    `relay/tests/conftest.py` documents) and makes `import app.*` resolve
    against `relay/` (already on `sys.path`, see module top)."""
    os.environ["FIRESTORE_EMULATOR_HOST"] = "localhost:8080"
    os.environ["FIREBASE_AUTH_EMULATOR_HOST"] = "localhost:9099"
    os.environ["GOOGLE_CLOUD_PROJECT"] = FIREBASE_PROJECT_ID


class Oracle:
    """Direct (whitebox) Firestore reads via `app.store.*`, used only to
    *verify* what the blackbox actions (through `pager_client`'s HTTP/MQTT
    calls) produced -- never to perform an action a real client couldn't.
    Bypasses `firestore.rules` (admin credentials), exactly like the relay
    itself does; that's the point of using it for assertions rather than
    hand-parsing the Firestore REST document shape."""

    def __init__(self) -> None:
        _use_relay_store()
        from app.store import messages as messages_store
        from app.store import users as users_store

        self.messages_store = messages_store
        self.users_store = users_store

    def uid_for_alias(self, alias: str) -> str:
        uid = self.users_store.get_uid_for_alias(alias)
        assert uid is not None, f"no such alias: {alias!r}"
        return uid

    def message(self, msg_id: str):
        return self.messages_store.get_message(msg_id)

    def thread(self, alias_a: str, alias_b: str):
        key = self.messages_store.conv_key(self.uid_for_alias(alias_a), self.uid_for_alias(alias_b))
        return self.messages_store.list_thread(key)


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------


def scenario_bootstrap() -> None:
    """Admin sign-in, parent + student users, an allow edge between them,
    and a pager device owned by the student with `parent` as its default
    recipient."""
    bootstrap_admin()

    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")

    admin.admin_user_add("parent", "Parent", email="parent@example.com", phone=None)
    admin.admin_user_add("student", "Student", email="student@example.com", phone=None)
    admin.admin_set_allow("parent", "student", message=True, locate=True, one_way=False)

    device_info = admin.admin_device_add("pgr-e2e-1", "student", default_to_alias="parent")
    assert device_info["mqttUsername"] == "pgr-e2e-1"
    assert device_info["mqttPassword"]
    print("bootstrap: admin, parent, student, allow-edge and device pgr-e2e-1 created")


def scenario_text_roundtrip() -> None:
    """parent -> student via the API -> the pager device (played by
    `pager_client`) receives it; `shown`/`read` acks update delivery state.
    student `msg` -> parent's thread; student `msg @dad` to a second
    parent."""
    oracle = Oracle()

    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")

    device = pager_client.DeviceClient("pgr-e2e-1", MQTT_HOST, MQTT_PORT, None, None)
    device.connect()
    wait_until(lambda: device.connected, timeout=10, description="device to connect")

    sent = parent.say("student", "pickup at 3:15 by the gym")
    msg_id = sent["id"]

    wait_until(
        lambda: any(e.data.get("id") == msg_id for e in device.inbox),
        timeout=10,
        description="device to receive the down message",
    )
    device.publish_ack(msg_id, "shown")
    device.publish_ack(msg_id, "read")

    def _is_read() -> bool:
        msg = oracle.message(msg_id)
        if msg is None:
            return False
        return any(d.kind == "pager" and d.state == "read" for d in msg.deliveries.values())

    wait_until(_is_read, timeout=10, description="pager delivery to reach 'read'")
    print("text_roundtrip: parent -> student -> device, shown+read acked")

    # student -> parent's thread (device up-message with no `to`, uses the
    # device's default recipient). The relay assigns a fresh `m_`-prefixed
    # Firestore doc id to the stored message; the device's own `u_`-prefixed
    # wire id survives only in `wireId` (PROTOCOL.md §1's dedup key), so the
    # match below is on `wireId`, not `id`.
    up_id = device.publish_msg("ok coming")
    wait_until(
        lambda: any(m.wireId == up_id for m in oracle.thread("parent", "student")),
        timeout=15,
        description="student's reply to land in the parent<->student thread",
    )

    # student `msg @dad` to a second parent.
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    admin.admin_user_add("dad", "Dad", email="dad@example.com", phone=None)
    admin.admin_set_allow("dad", "student", message=True, locate=True, one_way=False)

    up_id_2 = device.publish_msg("ok coming", to="dad")
    wait_until(
        lambda: any(m.wireId == up_id_2 for m in oracle.thread("dad", "student")),
        timeout=10,
        description="student's @dad reply to land in the dad<->student thread",
    )
    device.disconnect()
    print("text_roundtrip: student -> parent (default) and student -> @dad both landed")


def scenario_allowlist() -> None:
    """student `msg @stranger` (unregistered alias) -> dropped, `system`
    reply received by the device; admin denies parent<->student -> send
    returns 403; an unregistered signed-in UID gets 403 and cannot read
    Firestore."""
    device = pager_client.DeviceClient("pgr-e2e-1", MQTT_HOST, MQTT_PORT, None, None)
    device.connect()
    wait_until(lambda: device.connected, timeout=10, description="device to connect")
    device.inbox.clear()

    device.publish_msg("hi", to="stranger")
    wait_until(
        lambda: any(e.data.get("from") == "system" for e in device.inbox),
        timeout=10,
        description="a system 'unknown recipient' reply",
    )
    system_reply = next(e.data for e in device.inbox if e.data.get("from") == "system")
    assert system_reply["body"] == "unknown recipient"
    print("allowlist: unknown @stranger -> dropped + system reply received")

    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    admin.admin_deny("parent", "student")

    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")
    try:
        parent.say("student", "are you there?")
        raise AssertionError("expected say() to fail with 403 after admin_deny")
    except RuntimeError as exc:
        assert "403" in str(exc), f"expected a 403, got: {exc}"
    print("allowlist: admin deny -> send returns 403")

    # An unregistered signed-in UID: mint a real Firebase Auth account with
    # no users/{uid} doc, sign in, and confirm both the API and a direct
    # Firestore read reject it.
    from firebase_admin import auth as fb_auth

    _use_relay_store()
    from app.db.firestore import get_app

    get_app()
    stranger_uid = "e2e-stranger"
    try:
        fb_auth.get_user(stranger_uid)
    except fb_auth.UserNotFoundError:
        fb_auth.create_user(uid=stranger_uid, email="stranger@example.com")

    custom_token = fb_auth.create_custom_token(stranger_uid).decode("utf-8")
    exchange = httpx.post(
        f"{AUTH_URL}/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken",
        params={"key": pager_client.FAKE_API_KEY},
        json={"token": custom_token, "returnSecureToken": True},
        timeout=10.0,
    )
    exchange.raise_for_status()
    id_token = exchange.json()["idToken"]

    api_resp = httpx.get(f"{RELAY_URL}/api/me", headers={"Authorization": f"Bearer {id_token}"})
    assert api_resp.status_code == 403, api_resp.text

    fs_resp = httpx.get(
        f"http://localhost:8080/v1/projects/{FIREBASE_PROJECT_ID}/databases/(default)/documents/settings/retention",
        headers={"Authorization": f"Bearer {id_token}"},
        timeout=10.0,
    )
    assert fs_resp.status_code == 403, fs_resp.text
    device.disconnect()
    print("allowlist: unregistered signed-in uid -> 403 from both the API and Firestore")

    # Restore parent<->student (this scenario's `admin_deny` above is a
    # deliberate, permanent-until-changed admin action, same as it would be
    # against a real deployment) so later scenarios in the same run see the
    # allow-list state `bootstrap` set up, not this scenario's teardown.
    admin.admin_set_allow("parent", "student", message=True, locate=True, one_way=False)


def scenario_republish() -> None:
    """A device that has never connected still gets its queued message once
    it finally comes online (the online-edge republish, PROTOCOL.md §5.3) --
    chosen over "connect, then disconnect, then reconnect" for determinism:
    with a persistent MQTT session already subscribed, EMQX's own QoS 1
    queuing could also redeliver the message, which would make "arrives
    once" a race between two mechanisms rather than a clean test of the
    relay's own one. A brand-new device has no such session to race against.
    Separately: point the broker's REST API at nothing reachable (stop the
    `emqx` container) for one send -> delivery stays 'queued' -> bring EMQX
    back and call `/internal/tick` -> delivery gets published."""
    oracle = Oracle()
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    admin.admin_device_add("pgr-e2e-2", "student", default_to_alias="parent")

    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")

    sent = parent.say("student", "device is brand new, message queues")
    msg_id = sent["id"]

    def _is_queued_or_sent() -> bool:
        msg = oracle.message(msg_id)
        return msg is not None and any(d.kind == "pager" for d in msg.deliveries.values())

    wait_until(_is_queued_or_sent, timeout=10, description="pager delivery to be created")

    device = pager_client.DeviceClient("pgr-e2e-2", MQTT_HOST, MQTT_PORT, None, None)
    device.connect()
    wait_until(
        lambda: any(e.data.get("id") == msg_id for e in device.inbox),
        timeout=10,
        description="the never-connected device's first connect to trigger the online-edge republish",
    )
    matching = [e for e in device.inbox if e.data.get("id") == msg_id]
    assert len(matching) == 1, f"expected exactly one delivery of {msg_id}, got {len(matching)}"
    device.disconnect()
    print("republish: message queued before the device ever connected arrived exactly once")

    # --- broker REST API down -> tick recovers it ---
    print("republish: stopping emqx to simulate the broker's REST API being down...")
    compose("stop", "emqx")

    sent2 = parent.say("student", "broker is down for this one")
    msg_id_2 = sent2["id"]

    def _delivery_state(mid: str) -> str | None:
        msg = oracle.message(mid)
        if msg is None:
            return None
        for d in msg.deliveries.values():
            if d.kind == "pager":
                return d.state
        return None

    wait_until(
        lambda: _delivery_state(msg_id_2) == "queued",
        timeout=15,
        description="delivery to stay 'queued' while the broker is unreachable",
    )
    print("republish: delivery stayed 'queued' while emqx was down")

    compose("start", "emqx")
    wait_until(_emqx_reachable, timeout=90, interval=2, description="emqx's REST API to come back")
    configure_emqx()  # restarting the container loses its dynamic rule-engine config

    tick_resp = admin.api_post("/internal/tick", {})
    tick_resp.raise_for_status()
    print(f"republish: /internal/tick -> {tick_resp.json()}")

    wait_until(
        lambda: _delivery_state(msg_id_2) == "sent",
        timeout=15,
        description="delivery to reach 'sent' after tick retried it",
    )
    print("republish: /internal/tick delivered the previously-queued message")


SCENARIOS: dict[str, Callable[[], None]] = {
    "bootstrap": scenario_bootstrap,
    "text_roundtrip": scenario_text_roundtrip,
    "allowlist": scenario_allowlist,
    "republish": scenario_republish,
}


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "scenarios", nargs="*", default=list(SCENARIOS), help="scenario names to run, in order"
    )
    parser.add_argument("--no-build", action="store_true", help="skip `docker compose build`")
    args = parser.parse_args(argv)

    unknown = [s for s in args.scenarios if s not in SCENARIOS]
    if unknown:
        raise SystemExit(f"unknown scenario(s): {unknown}; choices: {list(SCENARIOS)}")

    start_stack(build=not args.no_build)
    passed: list[str] = []
    try:
        for name in args.scenarios:
            print(f"\n=== scenario: {name} ===")
            SCENARIOS[name]()
            passed.append(name)
        print(f"\nALL PASSED: {passed}")
        return 0
    except Exception:
        logger.exception("scenario failed after passing: %s", passed)
        dump_logs()
        return 1
    finally:
        stop_stack()


if __name__ == "__main__":
    sys.exit(main())

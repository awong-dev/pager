#!/usr/bin/env python3
"""
End-to-end integration suite for the v2 pager relay -- docs/SERVER_PLAN.md
§8. Brings up the real `relay/docker-compose.yml` stack (EMQX + Firebase
emulators + relay, `DEV_MODE=1`), imports `tools/pager_client.py` **as a
library** (one process plays the pager device *and* every human in the
scenario), and runs the named scenarios below: bootstrap, text round-trip,
allow-list and republish (1-4); location -- periodic fixes and read
permission, on-demand `/locate` incl. coalescing/cached-answer/`no_fix`/
derived-expiry (5-6); fan-out, retention and byte accounting (7-9).

Usage:
    python3 tools/e2e_v2.py                 # all scenarios
    python3 tools/e2e_v2.py bootstrap allowlist
    python3 tools/e2e_v2.py location_periodic location_on_demand

Exit code 0 if every requested scenario passes, 1 otherwise. Always tears
down the compose stack it started.

**Known gap -- EMQX device credentials are not provisioned:**
`tools/emqx_setup.py` and
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
otherwise the admin UI shows 'add this to the broker' copy"). A real
deployment must set the credentials and ACLs up in the broker console --
see `infra/README.md`.
"""

from __future__ import annotations

import argparse
import logging
import os
import re
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
# tools/mocks/twilio_mock.py, published on the host at the same
# port docker-compose.yml maps it to (relay/docker-compose.yml's
# `twilio-mock` service); the relay container itself reaches it at
# http://twilio-mock:8010 (set as TWILIO_BASE_URL in that same file).
TWILIO_MOCK_URL = "http://localhost:8010"
# docs/PROTOCOL.md §13.4: 900s (15 min) in production. Shortened for this
# whole compose run so `scenario_location_on_demand`'s derived-expiry
# sub-case doesn't wait 15 real minutes -- 10s is comfortably longer than
# every other sub-case's own timing (the coalescing sub-case's 3s
# artificial answer delay included), so it doesn't make any *other*
# scenario's loc_req go stale before it's answered.
LOC_REQ_TTL_S = 10

sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(RELAY_DIR))
import httpx
import pager_client

_env_backup: str | None = None


# --------------------------------------------------------------------------
# compose plumbing
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
                "BROKER_API_URL=http://emqx:18083/api/v5",
                f"BROKER_API_KEY={BROKER_API_KEY}",
                f"BROKER_API_SECRET={BROKER_API_SECRET}",
                f"WEBHOOK_KEY={WEBHOOK_KEY}",
                f"LOC_REQ_TTL_S={LOC_REQ_TTL_S}",
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


def _twilio_mock_reachable() -> bool:
    try:
        resp = httpx.get(f"{TWILIO_MOCK_URL}/healthz", timeout=3.0)
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
    wait_until(
        _twilio_mock_reachable, timeout=60, interval=1, description="twilio-mock to become reachable"
    )
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
    for service, tail in (
        ("relay", "120"),
        ("emqx", "60"),
        ("firebase", "40"),
        ("twilio-mock", "40"),
    ):
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
        from app.store import locations as locations_store
        from app.store import messages as messages_store
        from app.store import settings as settings_store
        from app.store import users as users_store

        self.messages_store = messages_store
        self.users_store = users_store
        self.locations_store = locations_store
        self.settings_store = settings_store

    def uid_for_alias(self, alias: str) -> str:
        uid = self.users_store.get_uid_for_alias(alias)
        assert uid is not None, f"no such alias: {alias!r}"
        return uid

    def loc_req_exists(self, device_id: str) -> bool:
        from app.db.firestore import get_db

        return get_db().collection("locReqs").document(device_id).get().exists

    def message(self, msg_id: str):
        return self.messages_store.get_message(msg_id)

    def thread(self, alias_a: str, alias_b: str):
        key = self.messages_store.conv_key(self.uid_for_alias(alias_a), self.uid_for_alias(alias_b))
        return self.messages_store.list_thread(key)


# --------------------------------------------------------------------------
# Backdating helpers -- scenario_retention (docs/SERVER_PLAN.md §5.7/§8
# scenario 8). There is no `pager_client`/relay-API way to "send a fix (or a
# message) from the past": a real device can only report the current time,
# and `POST /api/conversations/{alias}/messages` always stamps `createdAt`
# with `SERVER_TIMESTAMP`. These insert documents directly through the
# Firestore admin SDK (`app.db.firestore.get_db()`) -- the same whitebox
# access `Oracle` already uses for assertions elsewhere in this file -- with an
# explicit past `createdAt`, so the retention sweep's cutoff actually has
# something to bite on without waiting for a real retention window to elapse.
# --------------------------------------------------------------------------


def _backdate_location(device_id: str, *, days_ago: float) -> str:
    _use_relay_store()
    from datetime import UTC, datetime, timedelta

    from app.db.firestore import get_db

    ts = int(time.time())
    doc = {
        "ts": ts,
        "fixTs": ts,
        "lat": 37.0,
        "lon": -122.0,
        "accM": 5,
        "src": "gnss",
        "cached": False,
        "reqId": None,
        "createdAt": datetime.now(UTC) - timedelta(days=days_ago),
    }
    _, ref = get_db().collection("devices").document(device_id).collection("locations").add(doc)
    return ref.id


def _backdate_message(sender_uid: str, recipient_uid: str, *, days_ago: float) -> str:
    _use_relay_store()
    from datetime import UTC, datetime, timedelta

    from app.db.firestore import get_db
    from app.ids import new_id
    from app.store import messages as messages_store

    msg_id = new_id("m_")
    key = messages_store.conv_key(sender_uid, recipient_uid)
    doc = {
        "seq": 1,
        "convKey": key,
        "uids": sorted([sender_uid, recipient_uid]),
        "senderUid": sender_uid,
        "recipientUid": recipient_uid,
        "kind": "text",
        "body": "stale (backdated by tools/e2e_v2.py's scenario_retention)",
        "loc": None,
        "wireId": None,
        "originBackendKind": "webapp",
        "originBackendId": None,
        "ts": int(time.time()),
        "createdAt": datetime.now(UTC) - timedelta(days=days_ago),
        "deliveries": {},
        "pendingDeviceIds": [],
    }
    get_db().collection("messages").document(msg_id).set(doc)
    return msg_id


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


def scenario_location_periodic() -> None:
    """`loc auto 5` -> `devices/{d}/locations` fills over a few cycles
    (verified both whitebox, via `Oracle`, and through the API a real
    parent uses, `pager_client.ServerClient.locations`); periodic fixes
    never create a thread message (PROTOCOL.md §3.2); a user without
    `locate` permission cannot read them -- denied by `firestore.rules`'
    `locatableBy` check (§5.6), surfaced here as `locations()` finding no
    device it's allowed to see."""
    oracle = Oracle()
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    admin.admin_user_add(
        "locperiodic", "LocPeriodic", email="locperiodic@example.com", phone=None
    )
    # Device *before* allow-edge: `devices_store.create_device` always
    # starts a fresh device's `locatableBy` at `[]`;
    # it is only ever recomputed by a *later* allow-list write
    # (`allow_store.set_edge`/`replace_all`), which itself only touches
    # devices that already exist at the time it runs. Setting the edge
    # before the device exists would silently leave `locatableBy` empty
    # forever (until the allow-list happens to be rewritten again).
    admin.admin_device_add("pgr-e2e-locp", "locperiodic", default_to_alias="parent")
    admin.admin_set_allow("parent", "locperiodic", message=True, locate=True, one_way=True)

    device = pager_client.DeviceClient("pgr-e2e-locp", MQTT_HOST, MQTT_PORT, None, None)
    device.connect()
    wait_until(lambda: device.connected, timeout=10, description="loc-periodic device to connect")

    device.loc_auto(5)
    wait_until(
        lambda: len(oracle.locations_store.list_locations("pgr-e2e-locp")) >= 2,
        timeout=25,
        description="at least 2 periodic fixes to land in devices/{d}/locations",
    )
    device.loc_stop_auto()
    fixes = oracle.locations_store.list_locations("pgr-e2e-locp")
    print(f"location_periodic: {len(fixes)} periodic fixes landed in devices/{{d}}/locations")

    conv = oracle.messages_store.get_conversation(
        oracle.messages_store.conv_key(
            oracle.uid_for_alias("parent"), oracle.uid_for_alias("locperiodic")
        )
    )
    assert conv is None, "periodic fixes must never create a thread message (PROTOCOL.md §3.2)"

    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")
    via_api = parent.locations("locperiodic")
    assert len(via_api) >= 2, via_api
    print("location_periodic: a user with locate permission reads locations via Firestore REST")

    admin.admin_user_add(
        "locoutsider", "LocOutsider", email="locoutsider@example.com", phone=None
    )
    outsider = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    outsider.login("locoutsider")
    try:
        outsider.locations("locperiodic")
        raise AssertionError("expected locations() to be denied for a non-locate user")
    except RuntimeError as exc:
        print(f"location_periodic: user without locate permission denied: {exc}")

    device.disconnect()


def scenario_location_on_demand() -> None:
    """`locate` -> device answers with a real fix, and the request is
    fulfilled; a second `locate` within 60s is answered `cached:true` with
    no new wire message; two different requesters' `locate` calls within
    the coalescing window attach to the same `loc_req`, and both eventually
    get a `kind='loc'` thread message once the device answers; `loc fail
    on` -> the device answers `err:"no_fix"` and the relay still resolves
    the request to `fulfilled` (not stuck, not a crash); derived expiry,
    using the compose stack's shortened `LOC_REQ_TTL_S`, plus a `tick()`
    call, confirms a stale `locReqs` doc is cleared and a subsequent
    `/locate` starts fresh instead of coalescing onto the dead one."""
    oracle = Oracle()
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")

    # --- a real fix, then a <60s cached re-ask ---
    # Device before allow-edge in every sub-case below -- see
    # scenario_location_periodic's comment on the same ordering.
    admin.admin_user_add("locdemo1", "LocDemo1", email="locdemo1@example.com", phone=None)
    admin.admin_device_add("pgr-e2e-locd1", "locdemo1", default_to_alias="parent")
    admin.admin_set_allow("parent", "locdemo1", message=True, locate=True, one_way=True)
    device1 = pager_client.DeviceClient("pgr-e2e-locd1", MQTT_HOST, MQTT_PORT, None, None)
    device1.connect()
    wait_until(lambda: device1.connected, timeout=10, description="locdemo1 device to connect")

    outcome1 = parent.locate("locdemo1")
    assert outcome1["requestId"], outcome1
    assert outcome1["cached"] is False, outcome1

    def _fulfilled() -> bool:
        msg = oracle.message(outcome1["requestId"])
        if msg is None:
            return False
        return any(d.kind == "pager" and d.state == "fulfilled" for d in msg.deliveries.values())

    wait_until(_fulfilled, timeout=10, description="loc_req to be fulfilled by the device's answer")
    print("location_on_demand: locate -> device answered, loc_req fulfilled")

    loc_req_count_before = sum(1 for e in device1.inbox if e.data.get("kind") == "loc_req")
    outcome2 = parent.locate("locdemo1")
    assert outcome2["cached"] is True, outcome2
    assert outcome2["requestId"] is None, outcome2
    loc_req_count_after = sum(1 for e in device1.inbox if e.data.get("kind") == "loc_req")
    assert loc_req_count_after == loc_req_count_before, "cached answer must not re-ask the device"
    print("location_on_demand: second locate() within 60s answered cached:true, no new wire message")
    device1.disconnect()

    # --- coalescing: two requesters, one live loc_req ---
    admin.admin_user_add("locdemo2", "LocDemo2", email="locdemo2@example.com", phone=None)
    admin.admin_user_add(
        "locrequester2", "LocRequester2", email="locrequester2@example.com", phone=None
    )
    admin.admin_device_add("pgr-e2e-locd2", "locdemo2", default_to_alias="parent")
    admin.admin_set_allow("parent", "locdemo2", message=True, locate=True, one_way=True)
    admin.admin_set_allow("locrequester2", "locdemo2", message=True, locate=True, one_way=True)
    device2 = pager_client.DeviceClient("pgr-e2e-locd2", MQTT_HOST, MQTT_PORT, None, None)
    # A real device takes real time to attempt a fix (PROTOCOL.md §13.3
    # rule 2 bounds it at 60s); this simulator otherwise answers instantly,
    # which would make the coalescing window below a flaky race against two
    # separate HTTP round trips. A few real seconds of delay makes the test
    # deterministic without changing any relay-side behaviour under test.
    device2.loc_answer_delay_s = 3.0
    device2.connect()
    wait_until(lambda: device2.connected, timeout=10, description="locdemo2 device to connect")

    requester2 = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    requester2.login("locrequester2")

    outcomeA = parent.locate("locdemo2")
    outcomeB = requester2.locate("locdemo2")
    assert outcomeA["requestId"] == outcomeB["requestId"], (outcomeA, outcomeB)
    print("location_on_demand: two requesters coalesced onto the same loc_req")

    def _both_notified() -> bool:
        mom_thread = oracle.thread("parent", "locdemo2")
        req_thread = oracle.thread("locrequester2", "locdemo2")
        return any(m.kind == "loc" for m in mom_thread) and any(
            m.kind == "loc" for m in req_thread
        )

    wait_until(
        _both_notified,
        timeout=15,
        description="both coalesced requesters to get a kind='loc' message",
    )
    print("location_on_demand: both coalesced requesters received their own kind='loc' message")
    device2.disconnect()

    # --- loc fail on -> no_fix, still resolves the request ---
    admin.admin_user_add("locdemo3", "LocDemo3", email="locdemo3@example.com", phone=None)
    admin.admin_device_add("pgr-e2e-locd3", "locdemo3", default_to_alias="parent")
    admin.admin_set_allow("parent", "locdemo3", message=True, locate=True, one_way=True)
    device3 = pager_client.DeviceClient("pgr-e2e-locd3", MQTT_HOST, MQTT_PORT, None, None)
    device3.connect()
    wait_until(lambda: device3.connected, timeout=10, description="locdemo3 device to connect")
    device3.loc_set_fail(True)

    outcome3 = parent.locate("locdemo3")

    def _no_fix_fulfilled() -> bool:
        msg = oracle.message(outcome3["requestId"])
        if msg is None:
            return False
        return any(d.kind == "pager" and d.state == "fulfilled" for d in msg.deliveries.values())

    wait_until(_no_fix_fulfilled, timeout=10, description="no_fix answer to still fulfil the request")
    thread3 = oracle.thread("parent", "locdemo3")
    loc_msgs3 = [m for m in thread3 if m.kind == "loc"]
    assert len(loc_msgs3) == 1 and loc_msgs3[0].loc == {"err": "no_fix"}, loc_msgs3
    print("location_on_demand: loc fail on -> err:no_fix, request resolved to fulfilled (not stuck)")
    device3.disconnect()

    # --- derived expiry: shortened LOC_REQ_TTL_S + tick() ---
    admin.admin_user_add("locdemo4", "LocDemo4", email="locdemo4@example.com", phone=None)
    admin.admin_device_add("pgr-e2e-locd4", "locdemo4", default_to_alias="parent")
    admin.admin_set_allow("parent", "locdemo4", message=True, locate=True, one_way=True)
    # Deliberately never connected: PROTOCOL.md §5.3 excludes loc_req from
    # the online-edge republish ("a stale location request is worthless"),
    # so this request can never be answered -- exactly the
    # guaranteed-to-go-stale shape this sub-case needs.

    expiring = parent.locate("locdemo4")
    coalesced = parent.locate("locdemo4")
    assert coalesced["requestId"] == expiring["requestId"], "still within the TTL -> must coalesce"

    print(f"location_on_demand: waiting {LOC_REQ_TTL_S + 3}s for the loc_req to go stale...")
    time.sleep(LOC_REQ_TTL_S + 3)
    tick_resp = admin.api_post("/internal/tick", {})
    tick_resp.raise_for_status()
    tick_json = tick_resp.json()
    assert tick_json.get("locReqsCleared", 0) >= 1, tick_json
    assert not oracle.loc_req_exists("pgr-e2e-locd4")
    print(f"location_on_demand: tick() cleared the stale locReqs row -- {tick_json}")

    fresh = parent.locate("locdemo4")
    assert fresh["requestId"] != expiring["requestId"], "must not coalesce onto the expired request"
    print("location_on_demand: a locate() after expiry starts a fresh loc_req, not a coalesce")


def _sms_delivery_state(oracle: Oracle, msg_id: str) -> str | None:
    msg = oracle.message(msg_id)
    if msg is None:
        return None
    for d in msg.deliveries.values():
        if d.kind == "sms":
            return d.state
    return None


def scenario_fanout() -> None:
    """docs/SERVER_PLAN.md §8 scenario 7: a user with both `webapp` (implicit)
    and an `sms` backend enabled -> a message fans out to both; the mock
    (`tools/mocks/twilio_mock.py`) records the sms send; a forced mock
    failure drives the sms delivery through `/internal/tick`'s retry path
    (`app/jobs.py`) to `'failed'` after the attempts cap.

    **Origin-backend exclusion is exercised whitebox.** Driving it through
    the real inbound SMS webhook would need a signed Twilio request this
    suite has no credentials to produce. Per `app/routing.py`'s module
    docstring, the self-loop guard §5.2 means
    by "stops an SMS reply from being echoed back to the same phone" only
    ever fires when the *resolved recipient equals the sender* (see
    `relay/tests/test_routing.py`'s `test_origin_backend_id_scopes_self_loop_
    exclusion_to_exact_backend`, which this sub-case mirrors at the
    integration level) -- so this calls `app.routing.Routing.send()`
    directly (whitebox, like `Oracle`) with a self-addressed allow edge and
    `origin_backend_kind='sms'`/`origin_backend_id=<the user's own sms
    backend id>`, standing in for what a real inbound SMS webhook resolving
    a reply back to its own sender would pass, and confirms the
    sms delivery is excluded while webapp (a different kind) still gets one.
    """
    oracle = Oracle()
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    admin.admin_user_add("fanoutparent", "FanoutParent", email="fanoutparent@example.com", phone=None)
    admin.admin_user_add(
        "fanoutstudent", "FanoutStudent", email="fanoutstudent@example.com", phone=None
    )
    admin.admin_set_allow("fanoutparent", "fanoutstudent", message=True, locate=True, one_way=False)

    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("fanoutparent")
    sms_backend = parent.add_backend("sms", {"phone": "+15551234567"})
    assert sms_backend["enabled"] is False, sms_backend  # H2: not enabled until verified
    # `start_link()` already texted a verify code through the twilio mock
    # (`add_backend`'s call into `POST /api/me/backends` -- see that
    # handler's docstring) -- pull it back out of the mock's `/_sent` log
    # (H1: it is deliberately no longer readable off `sms_backend["config"]`)
    # and complete the verify flow before this scenario can rely on the
    # backend actually receiving anything.
    verify_sms = httpx.get(f"{TWILIO_MOCK_URL}/_sent", timeout=5.0)
    verify_sms.raise_for_status()
    verify_code_match = None
    for entry in reversed(verify_sms.json()):
        if entry["to"] == "+15551234567" and "verification code" in entry["body"]:
            verify_code_match = re.search(r"\d{6}", entry["body"])
            break
    assert verify_code_match is not None, verify_sms.json()
    verified = parent.verify_backend(sms_backend["id"], verify_code_match.group(0))
    assert verified["enabled"] is True and verified["verifiedAt"] is not None, verified

    student = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    student.login("fanoutstudent")

    # --- fan-out: webapp + sms both get deliveries ---
    sent = student.say("fanoutparent", "hi from student")
    msg_id = sent["id"]

    def _both_delivered() -> bool:
        msg = oracle.message(msg_id)
        if msg is None:
            return False
        states = {d.kind: d.state for d in msg.deliveries.values()}
        return states.get("webapp") == "sent" and states.get("sms") == "sent"

    wait_until(
        _both_delivered, timeout=15, description="webapp and sms deliveries to both reach 'sent'"
    )
    sent_sms = httpx.get(f"{TWILIO_MOCK_URL}/_sent", timeout=5.0)
    sent_sms.raise_for_status()
    assert any(
        s["to"] == "+15551234567" and s["body"] == "hi from student" for s in sent_sms.json()
    ), sent_sms.json()
    print("fanout: message fanned out to both webapp and sms deliveries; mock recorded the sms send")

    # --- origin-backend exclusion (documented simplification, see docstring above) ---
    _use_relay_store()
    from app.routing import Routing as _Routing
    from tests.fake_transport import FakeBrokerClient as _FakeBroker

    admin.admin_set_allow("fanoutparent", "fanoutparent", message=True, locate=True, one_way=True)
    loop_routing = _Routing(_FakeBroker())
    loop_result = loop_routing.send(
        sender_uid=oracle.uid_for_alias("fanoutparent"),
        recipient_alias="fanoutparent",
        kind="text",
        body="echo via sms",
        origin_backend_kind="sms",
        origin_backend_id=sms_backend["id"],
    )
    assert len(loop_result.messages) == 1, loop_result
    loop_kinds = {d.kind for d in loop_result.messages[0].deliveries.values()}
    assert "sms" not in loop_kinds, loop_kinds
    assert "webapp" in loop_kinds, loop_kinds
    print("fanout: sms origin backend excluded on a self-addressed reply; webapp still delivered")

    # --- retry path: mock forced to fail every send until the attempts cap ---
    fail_resp = httpx.post(f"{TWILIO_MOCK_URL}/_fail_next", json={"times": 10}, timeout=5.0)
    fail_resp.raise_for_status()
    sent2 = student.say("fanoutparent", "this sms keeps failing")
    msg_id_2 = sent2["id"]

    wait_until(
        lambda: _sms_delivery_state(oracle, msg_id_2) == "queued",
        timeout=10,
        description="sms delivery to stay 'queued' after the mock's forced failure",
    )
    print("fanout: mock /_fail_next -> sms delivery stayed 'queued' (not 'failed' outright)")

    # `MAX_DELIVERY_ATTEMPTS` (5, app/store/messages.py) -- the inline send
    # above already counts as attempt 1, so up to 4 more tick()-driven
    # retries reach the cap (same pattern relay/tests/test_jobs.py's
    # `test_tick_gives_up_after_five_failed_attempts` uses for pager).
    for _ in range(5):
        if _sms_delivery_state(oracle, msg_id_2) == "failed":
            break
        tick_resp = admin.api_post("/internal/tick", {})
        tick_resp.raise_for_status()
    assert _sms_delivery_state(oracle, msg_id_2) == "failed", _sms_delivery_state(oracle, msg_id_2)
    print("fanout: sms delivery retried via tick() and reached 'failed' after the attempts cap")


def scenario_retention() -> None:
    """docs/SERVER_PLAN.md §8 scenario 8 / §5.7.

    **Backdating approach**: see the
    `_backdate_location`/`_backdate_message` helpers above this file's
    Scenarios section -- there is no `pager_client`/relay-API way to send a
    fix or message dated in the past, so these insert documents directly
    through the Firestore admin SDK (`app.db.firestore.get_db()`) with an
    explicit past `createdAt`.

    **Resumability variant tested**: forcing `POST /internal/sweep`'s single
    HTTP request to abort partway through isn't something this script can
    trigger cleanly from outside the relay process without a second-process
    race with no clean way to synchronise it. Instead, the third sub-case
    below creates more than one batch's worth of stale messages and calls
    `sweep` twice in a row, confirming the second call deletes nothing --
    `relay/tests/test_jobs.py`'s `test_sweep_batches_correctly_with_a_small_
    sweep_batch`/`test_sweep_is_idempotent_across_two_consecutive_calls`
    cover the small-`SWEEP_BATCH` multi-iteration boundary itself at the
    unit level (an in-process `monkeypatch.setenv`, far more reliable than
    restarting the compose stack's relay container to change its env just
    for this one sub-case)."""
    oracle = Oracle()
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")

    # --- locations: retention.locations=1d, messages untouched ---
    admin.admin_user_add("retloc", "RetLoc", email="retloc@example.com", phone=None)
    admin.admin_device_add("pgr-e2e-retloc", "retloc")
    admin.admin_set_retention(messages="4w", locations="1d")

    fresh_loc_id = _backdate_location("pgr-e2e-retloc", days_ago=0.02)  # ~30 min old
    stale_loc_id = _backdate_location("pgr-e2e-retloc", days_ago=3)  # well past 1 day
    fresh_msg_id = parent.say("student", "retention: still here after a locations-only sweep")["id"]

    sweep_resp = admin.api_post("/internal/sweep", {})
    sweep_resp.raise_for_status()
    print(f"retention: sweep (locations=1d) -> {sweep_resp.json()}")

    remaining_ids = {f.id for f in oracle.locations_store.list_locations("pgr-e2e-retloc", limit=50)}
    assert stale_loc_id not in remaining_ids, remaining_ids
    assert fresh_loc_id in remaining_ids, remaining_ids
    assert oracle.message(fresh_msg_id) is not None, "messages must be untouched by a locations-only sweep"
    print("retention: stale location fix swept; fresh fix + messages untouched")

    # --- messages: retention.messages=2w ---
    admin.admin_set_retention(messages="2w", locations="1d")
    stale_msg_id = _backdate_message(
        oracle.uid_for_alias("parent"), oracle.uid_for_alias("student"), days_ago=20
    )
    fresh_msg_id_2 = parent.say("student", "retention: fresh message survives a 2-week sweep")["id"]

    sweep_resp2 = admin.api_post("/internal/sweep", {})
    sweep_resp2.raise_for_status()
    print(f"retention: sweep (messages=2w) -> {sweep_resp2.json()}")

    assert oracle.message(stale_msg_id) is None, "message older than the 2-week retention must be swept"
    assert oracle.message(fresh_msg_id_2) is not None
    print("retention: stale message swept; fresh message untouched")

    # --- idempotency: >1 batch's worth of stale docs, sweep() called twice ---
    stale_ids = [
        _backdate_message(oracle.uid_for_alias("parent"), oracle.uid_for_alias("student"), days_ago=20 + i)
        for i in range(3)
    ]
    sweep_resp3 = admin.api_post("/internal/sweep", {})
    sweep_resp3.raise_for_status()
    result3 = sweep_resp3.json()
    sweep_resp4 = admin.api_post("/internal/sweep", {})
    sweep_resp4.raise_for_status()
    result4 = sweep_resp4.json()
    print(f"retention: two sweep() calls in a row -> first {result3}, second {result4}")

    for mid in stale_ids:
        assert oracle.message(mid) is None
    assert result3["messagesDeleted"] >= len(stale_ids)
    assert result4["messagesDeleted"] == 0, "a second sweep() right after the first must be a no-op"
    print("retention: sweep() is idempotent -- the second call deleted nothing")


def scenario_bytes() -> None:
    """docs/SERVER_PLAN.md §8 scenario 9 -- informational only (no hard
    assertions beyond "it runs and prints something meaningful"). Connects
    a device, exercises a representative mix of up/down/status/loc traffic,
    then prints the same per-topic byte counters `pager_client.py`'s `bytes`
    command tracks, next to docs/PROTOCOL.md §7.2's raw-JSON-payload figures
    for comparison
    (§7.2's *MQTT bytes* column, before the TLS/TCP framing this local,
    non-TLS compose stack doesn't add -- the same "payload bytes only" thing
    `ByteCounter` measures)."""
    admin = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    admin.login("admin")
    admin.admin_user_add("bytesuser", "BytesUser", email="bytesuser@example.com", phone=None)
    admin.admin_device_add("pgr-e2e-bytes", "bytesuser", default_to_alias="parent")
    # Two-way (not `one_way=True` like the location scenarios) so the
    # device's own up-message below lands in the thread instead of tripping
    # the "unknown recipient"/"not allowed" system-reply path -- this
    # scenario wants representative traffic sizes, not an allow-list check.
    admin.admin_set_allow("parent", "bytesuser", message=True, locate=True, one_way=False)

    device = pager_client.DeviceClient("pgr-e2e-bytes", MQTT_HOST, MQTT_PORT, None, None)
    device.connect()
    wait_until(lambda: device.connected, timeout=10, description="bytes device to connect")

    parent = pager_client.ServerClient(RELAY_URL, AUTH_URL)
    parent.login("parent")
    sent = parent.say("bytesuser", "how are things going today?")
    wait_until(
        lambda: any(e.data.get("id") == sent["id"] for e in device.inbox),
        timeout=10,
        description="bytes device to receive the down message",
    )
    device.publish_ack(sent["id"], "shown")
    device.publish_ack(sent["id"], "read")
    device.publish_status(batt_mv=3150, mode="active", rssi=-80)
    device.loc_now(37.7749, -122.4194, acc=12)
    device.publish_msg("ok", to="parent")

    time.sleep(0.5)  # let the last up-message land before reading counters
    device.disconnect()

    published = device.bytes.published
    received = device.bytes.received
    assert sum(published.values()) > 0 and sum(received.values()) > 0, "expected some traffic counted"

    print("bytes: per-topic published bytes:", published)
    print("bytes: per-topic received bytes:", received)
    print(
        "bytes: PROTOCOL.md §7.2 reference payload sizes (JSON only, no TLS/TCP -- "
        "this compose stack has neither) -- /down 'msg' ~99 B, /down 'loc_req' ~78 B, "
        "/up ack ~51 B, /up reply ~84 B, /status ~117 B, /loc periodic ~160 B, "
        "/loc answering a loc_req ~160 B"
    )


SCENARIOS: dict[str, Callable[[], None]] = {
    "bootstrap": scenario_bootstrap,
    "text_roundtrip": scenario_text_roundtrip,
    "allowlist": scenario_allowlist,
    "republish": scenario_republish,
    "location_periodic": scenario_location_periodic,
    "location_on_demand": scenario_location_on_demand,
    "fanout": scenario_fanout,
    "retention": scenario_retention,
    "bytes": scenario_bytes,
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

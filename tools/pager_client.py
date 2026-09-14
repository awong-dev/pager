#!/usr/bin/env python3
"""
`pager_client.py` -- the pager v2 test client, docs/SERVER_PLAN.md §8.

Plays **both** roles a real deployment splits across a physical device and a
human's browser/phone: a simulated pager device (real MQTT, exactly what
firmware speaks -- the broker's rule-engine bridge to the relay is invisible
to it) and a server driver (HTTP against the relay's API + the Firestore
REST API with a real Firebase ID token). One process can therefore play "the
pager" and "the parent" for a whole end-to-end run -- see `tools/e2e_v2.py`.

Two ways to drive it:
  - stdlib `cmd` REPL:            python3 tools/pager_client.py
  - one-shot subcommand (scripting): python3 tools/pager_client.py msg "hi"
`--json` on either prints machine-readable output instead of prose.

Device-side flags: `--device-id --host --port --username --password`.
Server-side flags: `--api --auth-url --as <alias>` (`--as` is a convenience
that runs `login <alias>` before the REPL/subcommand).

This first cut implements docs/SERVER_PLAN.md §8's Phase 3 command set
(everything up to, but not including, location -- Phase 4) exactly:

  Device: connect, disconnect, crash, inbox, msg, ack, autoack, status, bytes
  Server: login, contacts, chat, say, watch, tick,
          admin user-add / allow / deny / device-add

Supersedes `tools/sim_device.py` conceptually (docs/SERVER_PLAN.md §8) --
`sim_device.py` itself is not deleted until Phase 5.
"""

from __future__ import annotations

import argparse
import cmd
import json
import os
import shlex
import sys
import time
from dataclasses import dataclass, field
from typing import Any

import httpx
import paho.mqtt.client as mqtt

DEFAULT_MQTT_HOST = os.environ.get("MQTT_BROKER_HOST", "localhost")
DEFAULT_MQTT_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
DEFAULT_API_URL = os.environ.get("PAGER_API_URL", "http://localhost:8000")
DEFAULT_AUTH_URL = os.environ.get("PAGER_AUTH_URL", "http://localhost:9099")
# The Auth emulator does not validate this key -- any non-empty string works
# (same convention as relay/tests/firebase_test_utils.py).
FAKE_API_KEY = "fake-api-key"
SIM_KEEPALIVE_S = 60


def new_id(prefix: str) -> str:
    """`prefix` + 8 lowercase hex chars, per docs/PROTOCOL.md §1."""
    return f"{prefix}{os.urandom(4).hex()}"


def now_ts() -> int:
    return int(time.time())


# ---------------------------------------------------------------------------
# Device side -- a real MQTT client speaking exactly what firmware speaks.
# ---------------------------------------------------------------------------


@dataclass
class ByteCounter:
    published: dict[str, int] = field(default_factory=dict)
    received: dict[str, int] = field(default_factory=dict)

    def add_published(self, topic: str, n: int) -> None:
        self.published[topic] = self.published.get(topic, 0) + n

    def add_received(self, topic: str, n: int) -> None:
        self.received[topic] = self.received.get(topic, 0) + n


@dataclass
class InboxEntry:
    topic: str
    data: dict[str, Any]
    received_at: float = field(default_factory=time.time)


class DeviceClient:
    """A simulated pager device: real MQTT, docs/PROTOCOL.md's exact wire
    shapes. `crash()` is the one thing a real device can't do to itself on
    purpose -- it exists to test PROTOCOL.md §5.3's online-edge republish."""

    def __init__(self, device_id: str, host: str, port: int, username: str | None, password: str | None):
        self.device_id = device_id
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.session_id = new_id("s_")
        self.connected = False
        self.inbox: list[InboxEntry] = []
        self.autoack: str = "off"  # off | on | shown-only
        self.bytes = ByteCounter()
        self._client: mqtt.Client | None = None
        self._loc_period_s = 0
        self._loc_min_s = 120

    @property
    def down_topic(self) -> str:
        return f"pager/{self.device_id}/down"

    @property
    def up_topic(self) -> str:
        return f"pager/{self.device_id}/up"

    @property
    def status_topic(self) -> str:
        return f"pager/{self.device_id}/status"

    def _build_client(self) -> mqtt.Client:
        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=self.device_id,
            clean_session=False,
            protocol=mqtt.MQTTv311,
        )
        if self.username:
            client.username_pw_set(self.username, self.password)
        lwt_payload = json.dumps(
            {"v": 1, "state": "offline", "session": self.session_id}, separators=(",", ":")
        )
        client.will_set(self.status_topic, lwt_payload, qos=1, retain=True)
        client.on_connect = self._on_connect
        client.on_message = self._on_message
        return client

    def connect(self) -> None:
        if self.connected:
            print(f"already connected as {self.device_id} (session={self.session_id})")
            return
        self._client = self._build_client()
        self._client.connect(self.host, self.port, keepalive=SIM_KEEPALIVE_S)
        self._client.loop_start()
        # Give the connect handshake + subscribe a moment to land before the
        # caller's next command publishes something.
        deadline = time.monotonic() + 5.0
        while not self.connected and time.monotonic() < deadline:
            time.sleep(0.05)
        if not self.connected:
            print("warning: connect timed out waiting for CONNACK", file=sys.stderr)

    def disconnect(self) -> None:
        if self._client is not None:
            self._client.loop_stop()
            self._client.disconnect()
        self.connected = False
        print(f"disconnected {self.device_id}")

    def crash(self) -> None:
        """A new cold-boot session id + reconnect -- PROTOCOL.md §1: session
        id changes per cold boot, not per deep-sleep wake. Tests the
        online-edge republish (§5.3), whose trigger is "session differs from
        the last seen session"."""
        self.disconnect()
        self.session_id = new_id("s_")
        self.connect()

    def _on_connect(self, client, userdata, connect_flags, reason_code, properties) -> None:
        self.connected = True
        client.subscribe(self.down_topic, qos=1)
        self.publish_status()

    def _on_message(self, client, userdata, msg) -> None:
        self.bytes.add_received(msg.topic, len(msg.payload))
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            print(f"<- received unparsable payload on {msg.topic}: {msg.payload[:64]!r}")
            return
        self.inbox.append(InboxEntry(topic=msg.topic, data=data))
        kind = data.get("kind", "msg")
        print(f"<- [{kind}] {data}")
        if kind == "loc_req":
            # PROTOCOL.md §3.2: MUST NOT ack, MUST NOT render -- Phase 4
            # answers on /loc; this client has no /loc support yet.
            return
        msg_id = data.get("id")
        if not msg_id:
            return
        if self.autoack == "on":
            self.publish_ack(msg_id, "shown")
            self.publish_ack(msg_id, "read")
        elif self.autoack == "shown-only":
            self.publish_ack(msg_id, "shown")

    def _publish(self, topic: str, obj: dict[str, Any], qos: int, retain: bool = False) -> None:
        assert self._client is not None, "not connected"
        payload = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        self.bytes.add_published(topic, len(payload))
        self._client.publish(topic, payload, qos=qos, retain=retain)

    def publish_status(
        self, *, batt_mv: int = 3280, mode: str = "sleep", rssi: int = -85, fw: str = "sim-0.2.0"
    ) -> None:
        self._publish(
            self.status_topic,
            {
                "v": 1,
                "state": "online",
                "mode": mode,
                "batt_mv": batt_mv,
                "rssi": rssi,
                "session": self.session_id,
                "ts": now_ts(),
                "fw": fw,
                "loc_period_s": self._loc_period_s,
                "loc_min_s": self._loc_min_s,
            },
            qos=1,
            retain=True,
        )

    def publish_ack(self, msg_id: str, ack: str) -> None:
        self._publish(self.up_topic, {"v": 1, "id": msg_id, "ts": now_ts(), "ack": ack}, qos=1)
        print(f"-> acked {ack} for {msg_id}")

    def publish_msg(self, body: str, to: str | None = None) -> str:
        msg_id = new_id("u_")
        obj: dict[str, Any] = {"v": 1, "id": msg_id, "ts": now_ts(), "from": self.device_id}
        if to:
            obj["to"] = to
        obj["body"] = body
        obj["ack"] = None
        self._publish(self.up_topic, obj, qos=1)
        print(f"-> sent u-message {msg_id}{' to ' + to if to else ''}: {body!r}")
        return msg_id


# ---------------------------------------------------------------------------
# Server side -- relay API + Firestore REST, with a real Firebase ID token.
# ---------------------------------------------------------------------------


class ServerClient:
    """Signs in like a real client would (dev-mode custom token -> Identity
    Toolkit exchange -> ID token), then drives the relay API and reads
    Firestore directly through its REST API -- the same path the web app's
    Firestore listeners use, so this also exercises `firestore.rules`."""

    def __init__(
        self,
        api_url: str,
        auth_url: str,
        project_id: str = "demo-pager",
        firestore_url: str | None = None,
    ):
        self.api_url = api_url.rstrip("/")
        self.auth_url = auth_url.rstrip("/")
        self.project_id = project_id
        # Firestore and Auth are separate emulators on separate ports
        # (docker-compose.yml: 8080 / 9099) -- default derives the Firestore
        # host from the auth URL's host rather than assuming "localhost",
        # so this still works when pointed at the compose network's
        # `firebase` service name.
        if firestore_url is None:
            from urllib.parse import urlsplit, urlunsplit

            parts = urlsplit(self.auth_url)
            host = parts.hostname or "localhost"
            firestore_url = urlunsplit((parts.scheme, f"{host}:8080", "", "", ""))
        self.firestore_url = firestore_url.rstrip("/")
        self.alias: str | None = None
        self.uid: str | None = None
        self._id_token: str | None = None
        self._http = httpx.Client(timeout=10.0)

    @property
    def firestore_base(self) -> str:
        return f"{self.firestore_url}/v1/projects/{self.project_id}/databases/(default)/documents"

    def _headers(self) -> dict[str, str]:
        if self._id_token is None:
            raise RuntimeError("not logged in -- run `login <alias>` first")
        return {"Authorization": f"Bearer {self._id_token}"}

    # ---- auth ----

    def login(self, alias: str) -> None:
        resp = self._http.post(f"{self.api_url}/api/dev/token", json={"alias": alias})
        if resp.status_code == 404:
            raise RuntimeError(
                f"login failed: unknown alias {alias!r} (or DEV_MODE is off on the relay)"
            )
        resp.raise_for_status()
        data = resp.json()
        custom_token = data["token"]
        self.uid = data["uid"]

        exchange = self._http.post(
            f"{self.auth_url}/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken",
            params={"key": FAKE_API_KEY},
            json={"token": custom_token, "returnSecureToken": True},
        )
        exchange.raise_for_status()
        self._id_token = exchange.json()["idToken"]
        self.alias = alias
        print(f"logged in as {alias} (uid={self.uid})")

    # ---- relay API ----

    def api_get(self, path: str) -> httpx.Response:
        return self._http.get(f"{self.api_url}{path}", headers=self._headers())

    def api_post(self, path: str, body: dict[str, Any]) -> httpx.Response:
        return self._http.post(f"{self.api_url}{path}", json=body, headers=self._headers())

    def api_delete(self, path: str) -> httpx.Response:
        return self._http.delete(f"{self.api_url}{path}", headers=self._headers())

    def api_put(self, path: str, body: dict[str, Any]) -> httpx.Response:
        return self._http.put(f"{self.api_url}{path}", json=body, headers=self._headers())

    def say(self, alias: str, text: str) -> dict[str, Any]:
        resp = self.api_post(f"/api/conversations/{alias}/messages", {"body": text})
        if resp.status_code >= 400:
            raise RuntimeError(f"say failed: {resp.status_code} {resp.text}")
        return resp.json()

    def tick(self) -> dict[str, Any]:
        resp = self.api_post("/internal/tick", {})
        resp.raise_for_status()
        return resp.json()

    # ---- admin ----

    def admin_user_add(
        self, alias: str, name: str, *, email: str | None, phone: str | None, admin: bool = False
    ) -> dict[str, Any]:
        body = {
            "alias": alias,
            "displayName": name,
            "email": email,
            "phone": phone,
            "role": "admin" if admin else "member",
            "uid": alias,  # see module docstring: uid == alias for this tool's users
        }
        resp = self.api_post("/api/admin/users", body)
        if resp.status_code >= 400:
            raise RuntimeError(f"admin user-add failed: {resp.status_code} {resp.text}")
        return resp.json()

    def admin_device_add(
        self, device_id: str, owner_alias: str, *, default_to_alias: str | None = None
    ) -> dict[str, Any]:
        body = {"deviceId": device_id, "ownerAlias": owner_alias, "label": device_id}
        if default_to_alias:
            body["defaultToAlias"] = default_to_alias
        resp = self.api_post("/api/admin/devices", body)
        if resp.status_code >= 400:
            raise RuntimeError(f"admin device-add failed: {resp.status_code} {resp.text}")
        return resp.json()

    def admin_set_allow(
        self, from_alias: str, to_alias: str, *, message: bool, locate: bool, one_way: bool
    ) -> None:
        entries = self._current_allowlist_entries()
        entries = [
            e
            for e in entries
            if not (
                (e["fromAlias"] == from_alias and e["toAlias"] == to_alias)
                or (not one_way and e["fromAlias"] == to_alias and e["toAlias"] == from_alias)
            )
        ]
        entries.append(
            {"fromAlias": from_alias, "toAlias": to_alias, "message": message, "locate": locate}
        )
        if not one_way:
            entries.append(
                {"fromAlias": to_alias, "toAlias": from_alias, "message": message, "locate": locate}
            )
        resp = self.api_put("/api/admin/allowlist", {"entries": entries})
        if resp.status_code >= 400:
            raise RuntimeError(f"admin allow failed: {resp.status_code} {resp.text}")

    def admin_deny(self, from_alias: str, to_alias: str) -> None:
        entries = self._current_allowlist_entries()
        entries = [
            e
            for e in entries
            if not (e["fromAlias"] == from_alias and e["toAlias"] == to_alias)
        ]
        resp = self.api_put("/api/admin/allowlist", {"entries": entries})
        if resp.status_code >= 400:
            raise RuntimeError(f"admin deny failed: {resp.status_code} {resp.text}")

    def _current_allowlist_entries(self) -> list[dict[str, Any]]:
        """`PUT /api/admin/allowlist` is replace-all (docs/SERVER_PLAN.md
        §5.1) -- `admin_set_allow`/`admin_deny` read the current edges back
        by alias first so a single `allow`/`deny` call doesn't clobber every
        other edge in the deployment. Aliases are resolved from the uids
        `GET /api/admin/allowlist` returns via `GET /api/admin/users`
        (admin-only reads, so this is only ever called by an admin-signed-in
        client)."""
        edges = self.api_get("/api/admin/allowlist").json()
        users = {u["uid"]: u["alias"] for u in self.api_get("/api/admin/users").json()}
        return [
            {
                "fromAlias": users.get(e["fromUid"], e["fromUid"]),
                "toAlias": users.get(e["toUid"], e["toUid"]),
                "message": e["message"],
                "locate": e["locate"],
            }
            for e in edges
        ]

    # ---- Firestore REST reads (exercises firestore.rules) ----

    def firestore_get(self, path: str) -> httpx.Response:
        return self._http.get(f"{self.firestore_base}/{path}", headers=self._headers())

    def firestore_run_query(self, body: dict[str, Any]) -> httpx.Response:
        return self._http.post(
            f"{self.firestore_base}:runQuery", json=body, headers=self._headers()
        )

    def contacts(self) -> list[str]:
        """Best-effort: reads `allow/{self}_*` via a structured query (the
        rule permits it: either party of an edge may read it), then tries to
        resolve each `toUid` to an alias via `users/{toUid}` -- which the
        rules only allow for *your own* uid or an admin, so a non-admin will
        see raw uids for contacts that aren't themselves. Good enough for a
        test client; the real web app instead reads `conversations/*`."""
        assert self.uid is not None
        query = {
            "structuredQuery": {
                "from": [{"collectionId": "allow"}],
                "where": {
                    "fieldFilter": {
                        "field": {"fieldPath": "fromUid"},
                        "op": "EQUAL",
                        "value": {"stringValue": self.uid},
                    }
                },
            }
        }
        resp = self.firestore_run_query(query)
        resp.raise_for_status()
        to_uids = []
        for row in resp.json():
            doc = row.get("document")
            if not doc:
                continue
            to_uids.append(doc["fields"]["toUid"]["stringValue"])
        aliases = []
        for uid in to_uids:
            user_resp = self.firestore_get(f"users/{uid}")
            if user_resp.status_code == 200:
                aliases.append(user_resp.json()["fields"]["alias"]["stringValue"])
            else:
                aliases.append(uid)
        return aliases

    def chat(self, alias: str, n: int = 20) -> list[dict[str, Any]]:
        """The thread with `alias`, oldest N -- a structured query on
        `messages` filtered by `convKey`, ordered by `seq`. Requires
        resolving `alias` to a uid first, which (same rules limitation as
        `contacts`) only works if it's this user's own uid or an admin's
        view; for the e2e suite every alias used here is either the caller
        or resolved via the admin token."""
        peer_uid = self._resolve_alias_best_effort(alias)
        conv_key = "_".join(sorted([self.uid, peer_uid]))
        query = {
            "structuredQuery": {
                "from": [{"collectionId": "messages"}],
                "where": {
                    "fieldFilter": {
                        "field": {"fieldPath": "convKey"},
                        "op": "EQUAL",
                        "value": {"stringValue": conv_key},
                    }
                },
                "orderBy": [{"field": {"fieldPath": "seq"}}],
                "limit": n,
            }
        }
        resp = self.firestore_run_query(query)
        resp.raise_for_status()
        out = []
        for row in resp.json():
            doc = row.get("document")
            if not doc:
                continue
            fields = doc["fields"]
            out.append(
                {
                    "id": doc["name"].rsplit("/", 1)[-1],
                    "senderUid": fields.get("senderUid", {}).get("stringValue"),
                    "body": fields.get("body", {}).get("stringValue"),
                }
            )
        return out

    def _resolve_alias_best_effort(self, alias: str) -> str:
        # Dev-mode-only shortcut: this tool always creates users with
        # uid == alias (see admin_user_add), so the common case needs no
        # lookup at all.
        return alias


# ---------------------------------------------------------------------------
# The combined shell
# ---------------------------------------------------------------------------


class PagerShell(cmd.Cmd):
    intro = "pager_client.py -- type `help` for commands, Ctrl-D to exit."
    prompt = "(pager) "

    def __init__(self, device: DeviceClient, server: ServerClient, *, as_json: bool = False):
        super().__init__()
        self.device = device
        self.server = server
        self.as_json = as_json

    def _out(self, obj: Any) -> None:
        if self.as_json:
            print(json.dumps(obj, default=str))
        else:
            print(obj)

    # ---- device commands ----

    def do_connect(self, arg: str) -> None:
        self.device.connect()

    def do_disconnect(self, arg: str) -> None:
        self.device.disconnect()

    def do_crash(self, arg: str) -> None:
        self.device.crash()

    def do_inbox(self, arg: str) -> None:
        rows = [
            {"topic": e.topic, "kind": e.data.get("kind", "msg"), **e.data} for e in self.device.inbox
        ]
        self._out(rows)

    def do_msg(self, arg: str) -> None:
        parts = shlex.split(arg)
        if not parts:
            print("usage: msg [@alias] <text>")
            return
        to = None
        if parts[0].startswith("@"):
            to = parts[0][1:]
            parts = parts[1:]
        body = " ".join(parts)
        self.device.publish_msg(body, to=to)

    def do_ack(self, arg: str) -> None:
        parts = arg.split()
        if len(parts) != 2 or parts[1] not in ("shown", "read"):
            print("usage: ack <id> shown|read")
            return
        self.device.publish_ack(parts[0], parts[1])

    def do_autoack(self, arg: str) -> None:
        mode = arg.strip()
        if mode not in ("on", "off", "shown-only"):
            print("usage: autoack on|off|shown-only")
            return
        self.device.autoack = mode
        print(f"autoack = {mode}")

    def do_status(self, arg: str) -> None:
        parser = argparse.ArgumentParser(prog="status", add_help=False)
        parser.add_argument("--batt", type=int, default=3280)
        parser.add_argument("--mode", choices=["sleep", "active"], default="sleep")
        parser.add_argument("--rssi", type=int, default=-85)
        try:
            ns = parser.parse_args(shlex.split(arg))
        except SystemExit:
            return
        self.device.publish_status(batt_mv=ns.batt, mode=ns.mode, rssi=ns.rssi)

    def do_bytes(self, arg: str) -> None:
        self._out({"published": self.device.bytes.published, "received": self.device.bytes.received})

    # ---- server commands ----

    def do_login(self, arg: str) -> None:
        alias = arg.strip()
        if not alias:
            print("usage: login <alias>")
            return
        self.server.login(alias)

    def do_contacts(self, arg: str) -> None:
        self._out(self.server.contacts())

    def do_chat(self, arg: str) -> None:
        parts = arg.split()
        if not parts:
            print("usage: chat <alias> [n]")
            return
        alias = parts[0]
        n = int(parts[1]) if len(parts) > 1 else 20
        self._out(self.server.chat(alias, n))

    def do_say(self, arg: str) -> None:
        parts = shlex.split(arg)
        if len(parts) < 2:
            print("usage: say <alias> <text>")
            return
        alias, text = parts[0], " ".join(parts[1:])
        self._out(self.server.say(alias, text))

    def do_watch(self, arg: str) -> None:
        alias = arg.strip()
        if not alias:
            print("usage: watch <alias>")
            return
        print(f"watching thread with {alias} (Ctrl-C to stop)...")
        seen: set[str] = set()
        try:
            while True:
                for m in self.server.chat(alias, 50):
                    if m["id"] not in seen:
                        seen.add(m["id"])
                        print(f"[{m['senderUid']}] {m['body']}")
                time.sleep(1.0)
        except KeyboardInterrupt:
            print("stopped watching")

    def do_tick(self, arg: str) -> None:
        self._out(self.server.tick())

    def do_admin(self, arg: str) -> None:
        parts = shlex.split(arg)
        if not parts:
            print("usage: admin user-add|device-add|allow|deny ...")
            return
        sub, rest = parts[0], parts[1:]
        if sub == "user-add":
            self._admin_user_add(rest)
        elif sub == "device-add":
            self._admin_device_add(rest)
        elif sub == "allow":
            self._admin_allow(rest)
        elif sub == "deny":
            self._admin_deny(rest)
        else:
            print(f"unknown admin subcommand: {sub}")

    def _admin_user_add(self, args: list[str]) -> None:
        parser = argparse.ArgumentParser(prog="admin user-add", add_help=False)
        parser.add_argument("alias")
        parser.add_argument("name")
        parser.add_argument("--email")
        parser.add_argument("--phone")
        parser.add_argument("--admin", action="store_true")
        try:
            ns = parser.parse_args(args)
        except SystemExit:
            return
        self._out(
            self.server.admin_user_add(
                ns.alias, ns.name, email=ns.email, phone=ns.phone, admin=ns.admin
            )
        )

    def _admin_device_add(self, args: list[str]) -> None:
        parser = argparse.ArgumentParser(prog="admin device-add", add_help=False)
        parser.add_argument("device_id")
        parser.add_argument("--owner", required=True)
        parser.add_argument("--default-to")
        try:
            ns = parser.parse_args(args)
        except SystemExit:
            return
        result = self.server.admin_device_add(
            ns.device_id, ns.owner, default_to_alias=ns.default_to
        )
        print(f"device {ns.device_id} created; mqtt password (shown once): {result['mqttPassword']}")
        self._out(result)

    def _admin_allow(self, args: list[str]) -> None:
        parser = argparse.ArgumentParser(prog="admin allow", add_help=False)
        parser.add_argument("a")
        parser.add_argument("b")
        parser.add_argument("--no-locate", action="store_true")
        parser.add_argument("--one-way", action="store_true")
        try:
            ns = parser.parse_args(args)
        except SystemExit:
            return
        self.server.admin_set_allow(
            ns.a, ns.b, message=True, locate=not ns.no_locate, one_way=ns.one_way
        )
        print(f"allowed {ns.a} -> {ns.b}{'' if ns.one_way else ' (and back)'}")

    def _admin_deny(self, args: list[str]) -> None:
        if len(args) != 2:
            print("usage: admin deny <a> <b>")
            return
        self.server.admin_deny(args[0], args[1])
        print(f"denied {args[0]} -> {args[1]}")

    # ---- misc ----

    def do_exit(self, arg: str) -> bool:
        return True

    def do_EOF(self, arg: str) -> bool:
        print()
        return True


# ---------------------------------------------------------------------------
# argparse: one-shot subcommand mode, shares PagerShell's do_* methods
# ---------------------------------------------------------------------------


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device-id", default=os.environ.get("PAGER_DEVICE_ID", "pgr-cli"))
    parser.add_argument("--host", default=DEFAULT_MQTT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_MQTT_PORT)
    parser.add_argument("--username", default=os.environ.get("MQTT_USERNAME"))
    parser.add_argument("--password", default=os.environ.get("MQTT_PASSWORD"))
    parser.add_argument("--api", default=DEFAULT_API_URL, help="relay API base URL")
    parser.add_argument("--auth-url", default=DEFAULT_AUTH_URL, help="Auth emulator base URL")
    parser.add_argument("--as", dest="as_alias", help="log in as this alias before running")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument(
        "command", nargs=argparse.REMAINDER, help="one-shot command (omit for the REPL)"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)

    device = DeviceClient(args.device_id, args.host, args.port, args.username, args.password)
    server = ServerClient(args.api, args.auth_url)
    shell = PagerShell(device, server, as_json=args.json)

    if args.as_alias:
        server.login(args.as_alias)

    if args.command:
        line = shlex.join(args.command)
        shell.onecmd(line)
        return 0

    shell.cmdloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

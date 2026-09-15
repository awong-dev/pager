#!/usr/bin/env python3
"""
`pager_client.py` -- the pager v2 test client, docs/SERVER_PLAN.md §8.

Plays **both** roles a real deployment splits across a physical device and a
human's browser/phone: a simulated pager device (real MQTT, exactly what
firmware speaks -- the broker's rule-engine bridge to the relay is invisible
to it) and a server driver (HTTP against the relay's API + the Firestore
REST API with a real Firebase ID token). One process can therefore play "the
pager" and "the parent" for a whole end-to-end run -- see `tools/e2e_v2.py`.

Needs `httpx` and `paho-mqtt`, so run it with the relay virtualenv's
interpreter -- a bare `python3` gives `ModuleNotFoundError: No module named
'httpx'` (see relay/README.md for creating the venv).

Two ways to drive it:
  - stdlib `cmd` REPL:               relay/.venv/bin/python tools/pager_client.py
  - one-shot subcommand (scripting): relay/.venv/bin/python tools/pager_client.py msg "hi"
`--json` on either prints machine-readable output instead of prose.

Device-side flags: `--device-id --host --port --username --password`.
Server-side flags: `--api --auth-url --as <alias>` (`--as` is a convenience
that runs `login <alias>` before the REPL/subcommand).

Implements docs/SERVER_PLAN.md §8's full command set -- text, location
(PROTOCOL.md §13), backends, retention and sweep -- plus docs/DEVICE_TASKS.md
S4.5's address book / cfg round trip (docs/DEVICE_PLAN.md §4, §5.8;
docs/PROTOCOL.md §3.2's `contact_req`/`book`/`cfg` kinds):

  Device: connect, disconnect, crash, inbox, msg, ack, autoack, status, bytes,
          loc <lat> <lon> [acc] | loc auto <period_s> [--walk] |
          loc min <s> | loc fail on|off,
          contactreq <name> [phone-or-alias]
  Server: login, contacts, chat, say, watch, tick, sweep, locate <alias>,
          locations <alias> [n],
          admin user-add / allow / deny / device-add /
          settings retention messages=<n><d|w> locations=<n><d|w> /
          contacts [pending|approved|rejected] / approve <key> link|create [alias] [--locate] /
          reject <key> <reason> / cfg <device_id> [--auto <min>] [--clear],
          backend add <kind> <json-config>

`DeviceClient.book`/`.lock` hold this simulated device's own applied state
(docs/PROTOCOL.md §3.2: `book`/`cfg` are "not a thread entry ... acked
`shown` once applied") -- see `_handle_book`/`_handle_cfg` below for exactly
what "applied" means for this simulator.
"""

from __future__ import annotations

import argparse
import base64
import cmd
import json
import os
import re
import shlex
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

import httpx
import paho.mqtt.client as mqtt

# docs/DEVICE_TASKS.md T1.5: the simulated device signs with the same
# `app/devauth.py`/`app/wirecbor.py` this repo's relay verifies with --
# rather than reimplementing HMAC/CBOR framing a second time here, which
# would only let a bug in one implementation hide behind a matching bug in
# the other. Needs `relay/` on `sys.path`; this file is documented to run
# under the relay venv already (see module docstring), so this is the only
# extra setup required -- `tools/e2e_v2.py` does the same thing for its own
# `import app.*` whitebox helpers.
REPO_ROOT = Path(__file__).resolve().parent.parent
RELAY_DIR = REPO_ROOT / "relay"
if str(RELAY_DIR) not in sys.path:
    sys.path.insert(0, str(RELAY_DIR))

from app import devauth, wirecbor

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


def parse_retention_shorthand(value: str) -> dict[str, Any]:
    """`4w` -> `{"n": 4, "unit": "weeks"}`, `10d` -> `{"n": 10, "unit":
    "days"}` -- docs/SERVER_PLAN.md §8's `admin settings retention
    messages=4w locations=10d` shorthand, matching §5.7/§3's
    `{n, unit: 'days'|'weeks'}` setting shape 1:1 so this is the only place
    that shorthand needs parsing."""
    m = re.fullmatch(r"(\d+)([dw])", value.strip().lower())
    if not m:
        raise ValueError(f"invalid retention shorthand {value!r} (expected e.g. '4w' or '10d')")
    n = int(m.group(1))
    unit = "weeks" if m.group(2) == "w" else "days"
    return {"n": n, "unit": unit}


def _fs_value(field: dict[str, Any] | None) -> Any:
    """Unwraps one Firestore REST API typed field value
    (`{"doubleValue": 1.0}`, `{"integerValue": "3"}`, `{"booleanValue":
    true}`, `{"stringValue": "x"}`, `{"nullValue": None}`) into a plain
    Python value -- just enough of the wire shape for `locations()` below,
    not a general-purpose decoder."""
    if not field:
        return None
    if "doubleValue" in field:
        return field["doubleValue"]
    if "integerValue" in field:
        return int(field["integerValue"])
    if "booleanValue" in field:
        return field["booleanValue"]
    if "stringValue" in field:
        return field["stringValue"]
    if "nullValue" in field:
        return None
    return None


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


# docs/PROTOCOL.md §14.2: n = (epoch << 20) | lo -- 20 bits of `lo` per
# epoch, 12 bits of epoch left over in the 32-bit counter.
_UP_LO_BITS = 20
_UP_LO_MASK = (1 << _UP_LO_BITS) - 1

# docs/DEVICE_TASKS.md T1.5's own instruction ("a 32-wide down window"),
# narrower than the relay's 64-wide `deviceSecrets.upBits` window
# (app/store/device_secrets.py, docs/PROTOCOL.md §14.2) -- a device-side
# implementation choice (how much RTC a real device spends on this), not a
# wire-format rule, so there is nothing for the two widths to disagree
# about; the accept/reject arithmetic itself mirrors
# `device_secrets.accept_up_n` exactly, just over a 32- instead of 64-bit
# bitmap.
_DOWN_WINDOW = 32
_DOWN_WINDOW_MASK = (1 << _DOWN_WINDOW) - 1


class DeviceClient:
    """A simulated pager device: real MQTT, docs/PROTOCOL.md's exact wire
    shapes. `crash()` is the one thing a real device can't do to itself on
    purpose -- it exists to test PROTOCOL.md §5.3's online-edge republish.

    `hmac_key` (docs/DEVICE_PLAN.md §2.3, `docs/PROTOCOL.md` §14): `None`
    (default) plays a v1 `authMode: "password"` device -- every publish goes
    out unsigned, exactly as before this task. A non-empty key plays an
    `authMode: "hmac"` device: every publish on `/up`, `/status` and `/loc`
    gets a fresh `n` (this device's own epoch/lo counter, §14.2) and `sig`
    (`app/devauth.py`), and every inbound `/down` is verified and dropped
    (logged, not raised) on a bad signature or a replayed/out-of-window `n`
    -- `_accept_down_n` below, this device's own mirror of the relay's
    replay window (§14.2's device-side rules), just 32 wide instead of 64
    (see `_DOWN_WINDOW`'s docstring).

    `wire` selects the encoding for every envelope this device publishes
    (first-byte dispatch on receipt, `app/wirecbor.py`) -- independent of
    `hmac_key`: encoding and signing are orthogonal per §14."""

    def __init__(
        self,
        device_id: str,
        host: str,
        port: int,
        username: str | None,
        password: str | None,
        *,
        hmac_key: bytes | None = None,
        wire: Literal["json", "cbor"] = "json",
    ):
        self.device_id = device_id
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.hmac_key = hmac_key
        self.wire = wire
        self.session_id = new_id("s_")
        self.connected = False
        self.inbox: list[InboxEntry] = []
        self.autoack: str = "off"  # off | on | shown-only
        self.bytes = ByteCounter()
        self._client: mqtt.Client | None = None
        self._loc_period_s = 0
        self._loc_min_s = 120
        # §14.2 device-side `/up`, `/status`, `/loc` counter: `n = (epoch <<
        # 20) | lo`. This process plays one cold boot per `DeviceClient`
        # instance (epoch/lo start at 0) and one more per `crash()` (a real
        # device's RTC-invalid cold-boot path -- see `crash()`).
        self._up_epoch = 0
        self._up_lo = 0
        # §14.2 device-side `/down` replay window: highest accepted `n` +
        # a bitmap of the `_DOWN_WINDOW` values below it, same shape as the
        # relay's own `deviceSecrets.upN`/`upBits`
        # (app/store/device_secrets.py).
        self._down_n = 0
        self._down_bits = 0
        # docs/DEVICE_TASKS.md S4.5 / docs/PROTOCOL.md §3.2 `kind:"book"`:
        # the last applied address book, or `None` before the device has
        # ever received one (a fresh device has no default recipient and no
        # approved contacts). Shape mirrors the wire envelope's own
        # `bv`/`d`/`c`/`p` fields (`_handle_book` below), which is also what
        # `book.c[].a` a caller like `tools/e2e_v2.py` reads to confirm an
        # alias landed.
        self.book: dict[str, Any] | None = None
        # docs/DEVICE_PLAN.md §5.8 `cfg` `lock` map, applied cumulatively:
        # `auto` persists at its last-set value, `clear` is a one-shot
        # action recorded as `lock_cleared_count` (how many `cfg lock:
        # {clear:true}` this device has applied) rather than a boolean, so a
        # test can distinguish "never cleared" from "cleared, still
        # unlocked" without needing a real passcode/lock state machine this
        # simulator doesn't otherwise model (out of scope for S4.5, which
        # only needs the ack + the value landing).
        self.lock_auto_min: int | None = None
        self.lock_cleared_count: int = 0
        # docs/PROTOCOL.md §13.3 -- device-side location state. `_lat`/`_lon`
        # is the device's current position (what the next fix attempt
        # reports); `_last_fix` is the last *successful* fix (what a
        # rate-limited `loc_req` answers from, cached); `_last_attempt_ts`
        # is the last fix *attempt* (successful or not -- rule 1's window
        # runs from the attempt, not the success, "so a device in a
        # basement cannot be made to retry continuously").
        self._lat = 37.7749
        self._lon = -122.4194
        self._loc_acc: int | None = None
        self._loc_fail = False
        self._last_fix: dict[str, Any] | None = None
        self._last_attempt_ts: float | None = None
        # Rule 3: "at most one fix attempt is in flight" -- this simulator's
        # attempts are instantaneous (no real GNSS to wait on), so the lock
        # only needs to serialise concurrent MQTT-thread callbacks (an
        # `auto` timer firing at the same moment a `loc_req` arrives); it
        # still gives every caller "the same result" per rule 3 because
        # there is never a window where two attempts are actually racing.
        self._loc_lock = threading.Lock()
        self._loc_walk = False
        self._auto_stop: threading.Event | None = None
        self._auto_thread: threading.Thread | None = None
        # Not part of PROTOCOL.md §13.3's normative command set -- a test-
        # only knob (`tools/e2e_v2.py` sets it directly, no REPL command)
        # standing in for the real seconds a GNSS attempt takes (rule 2
        # bounds it at 60s). This simulator otherwise answers a `loc_req`
        # instantaneously, which makes a coalescing test a flaky race
        # against two separate HTTP round trips instead of a deterministic
        # check of the relay's own behaviour; see `_handle_loc_req`.
        self.loc_answer_delay_s: float = 0.0

    @property
    def down_topic(self) -> str:
        return f"pager/{self.device_id}/down"

    @property
    def up_topic(self) -> str:
        return f"pager/{self.device_id}/up"

    @property
    def status_topic(self) -> str:
        return f"pager/{self.device_id}/status"

    @property
    def loc_topic(self) -> str:
        return f"pager/{self.device_id}/loc"

    def _build_client(self) -> mqtt.Client:
        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=self.device_id,
            clean_session=False,
            protocol=mqtt.MQTTv311,
        )
        if self.username:
            client.username_pw_set(self.username, self.password)
        # docs/PROTOCOL.md §14.6: the broker-generated LWT can never carry a
        # signature (and carries no `n`) regardless of `authMode` -- built
        # directly here, not through `_publish`, so it never picks either up.
        lwt_obj = {"v": 1, "state": "offline", "session": self.session_id}
        lwt_payload = self._encode(lwt_obj)
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
        self.loc_stop_auto()
        if self._client is not None:
            self._client.loop_stop()
            self._client.disconnect()
        self.connected = False
        print(f"disconnected {self.device_id}")

    def crash(self) -> None:
        """A new cold-boot session id + reconnect -- PROTOCOL.md §1: session
        id changes per cold boot, not per deep-sleep wake. Tests the
        online-edge republish (§5.3), whose trigger is "session differs from
        the last seen session". Also the device-side equivalent of a
        cold boot (§14.2: RTC invalid) -- bumps the `n` epoch so the relay's
        replay window absorbs the jump instead of seeing `lo` restart from
        under its current `upN`."""
        self.disconnect()
        self.session_id = new_id("s_")
        if self.hmac_key:
            self._up_epoch += 1
            self._up_lo = 0
        self.connect()

    def _on_connect(self, client, userdata, connect_flags, reason_code, properties) -> None:
        self.connected = True
        client.subscribe(self.down_topic, qos=1)
        self.publish_status()

    # ---- wire encoding + signing (docs/PROTOCOL.md §14, §3/§10) ----

    def _encode(self, obj: dict[str, Any]) -> bytes:
        """Encodes `obj` in this device's chosen wire encoding, unsigned --
        used only for the LWT (§14.6), which never carries `n`/`sig`."""
        if self.wire == "cbor":
            return wirecbor.encode(obj)
        return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

    def _decode(self, payload: bytes) -> dict[str, Any] | None:
        """Inverse of `_encode`, first-byte dispatch per §3/`wirecbor.is_cbor`
        -- `None` on anything that doesn't parse."""
        try:
            if wirecbor.is_cbor(payload):
                return wirecbor.decode(payload)
            return json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError, KeyError):
            return None

    def _next_up_n(self) -> int:
        """§14.2: `n = (epoch << 20) | lo`, `lo` incrementing per publish and
        wrapping into `epoch` -- this process's stand-in for a device's
        RTC `lo` + NVS `epoch`. Pre-increments (the first call returns `n=1`,
        never `n=0`): the relay's replay window (§14.2, `app/store/
        device_secrets.py`) uses `upN=0` as its own "nothing accepted yet"
        sentinel, so `n=0` can never be the value that first moves it off
        that sentinel -- it would read as a replay of a top that was never
        really there, not as this device's genuine first envelope."""
        self._up_lo += 1
        if self._up_lo > _UP_LO_MASK:
            self._up_lo = 1
            self._up_epoch += 1
        return (self._up_epoch << _UP_LO_BITS) | self._up_lo

    def _accept_down_n(self, n: int) -> bool:
        """§14.2's device-side `/down` replay window, mirroring the relay's
        own `app/store/device_secrets.accept_up_n` arithmetic exactly, just
        `_DOWN_WINDOW` wide instead of 64 (see that constant's docstring)."""
        if n > self._down_n:
            shift = n - self._down_n
            if shift >= _DOWN_WINDOW:
                self._down_bits = 0
            else:
                self._down_bits = ((self._down_bits << shift) | (1 << (shift - 1))) & _DOWN_WINDOW_MASK
            self._down_n = n
            return True
        gap = self._down_n - n
        if 0 < gap <= _DOWN_WINDOW:
            bit = 1 << (gap - 1)
            if self._down_bits & bit:
                return False
            self._down_bits |= bit
            return True
        return False

    def _on_message(self, client, userdata, msg) -> None:
        self.bytes.add_received(msg.topic, len(msg.payload))
        payload = msg.payload
        if self.hmac_key:
            ok, unsigned = devauth.verify(self.hmac_key, msg.topic, payload)
            if not ok:
                print(f"<- SECURITY dropped bad-sig/unsigned payload on {msg.topic}: {payload[:64]!r}")
                return
            data = self._decode(unsigned)
            if data is None:
                print(f"<- dropped unparsable (post-verify) payload on {msg.topic}: {unsigned[:64]!r}")
                return
            n = data.get("n")
            if not isinstance(n, int) or not self._accept_down_n(n):
                print(f"<- SECURITY dropped replayed/out-of-window /down n={n!r} on {msg.topic}")
                return
        else:
            data = self._decode(payload)
            if data is None:
                print(f"<- received unparsable payload on {msg.topic}: {payload[:64]!r}")
                return
        self.inbox.append(InboxEntry(topic=msg.topic, data=data))
        kind = data.get("kind", "msg")
        print(f"<- [{kind}] {data}")
        if kind == "loc_req":
            # PROTOCOL.md §3.2: MUST NOT ack, MUST NOT render -- answered on
            # /loc instead, subject to §13.3's device-side rate limit.
            self._handle_loc_req(data.get("id"))
            return
        if kind == "book":
            # §3.2 `kind:"book"`: not a thread entry, applied then acked
            # `shown` unconditionally -- unlike a `msg`, whose `shown` ack
            # depends on `autoack`/a simulated button press, a book's ack
            # reports "applied to NVS", which this simulator does
            # synchronously on receipt, every time.
            self._handle_book(data)
            return
        if kind == "cfg":
            self._handle_cfg(data)
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
        """§14.3: for an `authMode: hmac` device (`self.hmac_key` set), every
        publish on `/up`, `/status` and `/loc` (the only topics this class
        ever publishes to -- `/down` is inbound-only) gets a fresh `n`
        (`_next_up_n`) and `sig` (`app/devauth.py`), in this device's chosen
        wire encoding. Unsigned otherwise (`authMode: password`)."""
        assert self._client is not None, "not connected"
        if self.hmac_key:
            signed_obj = {**obj, "n": self._next_up_n()}
            if self.wire == "cbor":
                payload = devauth.sign_cbor(self.hmac_key, topic, signed_obj)
            else:
                payload = devauth.sign_json(self.hmac_key, topic, signed_obj)
        else:
            payload = self._encode(obj)
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

    # ---- address book / cfg (docs/PROTOCOL.md §3.2 `contact_req`/`book`/
    # `cfg`, docs/DEVICE_PLAN.md §4 (address book), §5.8 (device lock)) ----

    def publish_contact_req(self, name: str, ph: str | None = None) -> str:
        """`/up kind:"contact_req"` -- docs/PROTOCOL.md §3.2, §4.2: `name` is
        the display name, `ph` is *either* an E.164 phone number (leading
        `+`) or an alias reference (docs/ingest.py's `ContactReqEnvelope`
        docstring: the field is overloaded, distinguished by the `+`
        prefix) -- this simulator passes whatever the caller gives it
        straight through, unvalidated, the same "device doesn't police its
        own wire shape beyond what it can trivially construct" stance
        `publish_msg` already takes. No `body`/`from` per §3.2's shape."""
        req_id = new_id("u_")
        obj: dict[str, Any] = {
            "v": 1,
            "id": req_id,
            "ts": now_ts(),
            "kind": "contact_req",
            "name": name,
            "ack": None,
        }
        if ph:
            obj["ph"] = ph
        self._publish(self.up_topic, obj, qos=1)
        print(f"-> contact_req {req_id}: name={name!r} ph={ph!r}")
        return req_id

    def _handle_book(self, data: dict[str, Any]) -> None:
        """docs/PROTOCOL.md §3.2 `kind:"book"`: "acked `shown` once the book
        has been applied ... atomically written to NVS." This simulator's
        NVS is just `self.book`, so "applied" is "assigned", synchronously,
        before the ack goes out -- there is no intermediate state where an
        observer could see the ack without the new book already in place."""
        self.book = {
            "bv": data.get("bv", 0),
            "d": data.get("d"),
            "c": data.get("c", []),
            "p": data.get("p", []),
        }
        msg_id = data.get("id")
        print(
            f"-> book applied: bv={self.book['bv']} d={self.book['d']!r} "
            f"contacts={[c.get('a') for c in self.book['c']]} "
            f"pending={[p.get('n') for p in self.book['p']]}"
        )
        if msg_id:
            self.publish_ack(msg_id, "shown")

    def _handle_cfg(self, data: dict[str, Any]) -> None:
        """docs/PROTOCOL.md §3.2 `kind:"cfg"` / docs/DEVICE_PLAN.md §5.8:
        applies `cfg.lock` (unknown `cfg` members ignored, per §3.2) and
        acks `shown`, same "applied before acked" rule as `_handle_book`.
        `lock.auto` persists at its last-set value; `lock.clear` is a
        one-shot action counted in `lock_cleared_count` (see that field's
        docstring) rather than modelled as a real passcode/lock state
        machine, which is out of this simulator's scope."""
        lock = (data.get("cfg") or {}).get("lock") or {}
        if lock.get("clear"):
            self.lock_cleared_count += 1
        if lock.get("auto") is not None:
            self.lock_auto_min = lock["auto"]
        msg_id = data.get("id")
        print(f"-> cfg applied: lock={lock!r} (auto_min now {self.lock_auto_min})")
        if msg_id:
            self.publish_ack(msg_id, "shown")

    # ---- location (PROTOCOL.md §13) ----

    def _take_fix(self) -> dict[str, Any] | None:
        """One fix attempt: `None` (no fix) iff `loc fail on`, otherwise the
        device's current position. Always records the attempt in
        `_last_fix`/`_last_attempt_ts` is the caller's job (both the
        periodic and on-demand paths need to update `_last_attempt_ts` at
        the moment of the *attempt*, per §13.3 rule 1, not just on
        success)."""
        if self._loc_fail:
            return None
        fix: dict[str, Any] = {"lat": self._lat, "lon": self._lon, "fix_ts": now_ts(), "src": "gnss"}
        if self._loc_acc is not None:
            fix["acc"] = self._loc_acc
        self._last_fix = fix
        return fix

    def publish_loc(
        self, *, req: str | None, loc: dict[str, Any] | None, cached: bool, err: str | None = None
    ) -> str:
        loc_id = new_id("l_")
        obj: dict[str, Any] = {"v": 1, "id": loc_id, "ts": now_ts(), "loc": loc, "req": req}
        if cached:
            obj["cached"] = True
        if err:
            obj["err"] = err
        # PROTOCOL.md §2: QoS 1 when `req` is non-null (an answer someone is
        # waiting on), QoS 0 otherwise (an unsolicited periodic fix).
        qos = 1 if req is not None else 0
        self._publish(self.loc_topic, obj, qos=qos)
        print(f"-> loc {loc_id} req={req} cached={cached} err={err} loc={loc}")
        return loc_id

    def loc_now(self, lat: float, lon: float, acc: int | None = None) -> None:
        """`loc <lat> <lon> [acc]`: one periodic fix (`req:null`) right now
        at the given position -- also updates the device's current position
        for future fixes (`loc auto`, a rate-limited `loc_req` answer)."""
        with self._loc_lock:
            self._lat, self._lon, self._loc_acc = lat, lon, acc
            fix = self._take_fix()
            self._last_attempt_ts = time.time()
        if fix is None:
            self.publish_loc(req=None, loc=None, cached=False, err="no_fix")
        else:
            self.publish_loc(req=None, loc=fix, cached=False)

    def loc_auto(self, period_s: int, walk: bool = False) -> None:
        """`loc auto <period_s> [--walk]`: periodic fixes (`req:null`) every
        `period_s` seconds until `loc_stop_auto()`/`disconnect()`/`crash()`.
        `--walk` drifts the position ~1 m/s northward between fixes (1
        degree of latitude is ~111km, so `period_s` seconds of drift is
        `period_s / 111_000` degrees)."""
        self.loc_stop_auto()
        self._loc_period_s = period_s
        self._loc_walk = walk
        if period_s <= 0:
            return
        self._auto_stop = threading.Event()
        stop_event = self._auto_stop

        def _loop() -> None:
            while not stop_event.wait(period_s):
                with self._loc_lock:
                    if self._loc_walk:
                        self._lat += period_s / 111_000.0
                    fix = self._take_fix()
                    self._last_attempt_ts = time.time()
                if fix is None:
                    self.publish_loc(req=None, loc=None, cached=False, err="no_fix")
                else:
                    self.publish_loc(req=None, loc=fix, cached=False)

        self._auto_thread = threading.Thread(target=_loop, daemon=True)
        self._auto_thread.start()

    def loc_stop_auto(self) -> None:
        self._loc_period_s = 0
        if self._auto_stop is not None:
            self._auto_stop.set()
        if self._auto_thread is not None:
            self._auto_thread.join(timeout=2.0)
        self._auto_thread = None
        self._auto_stop = None

    def loc_set_min(self, seconds: int) -> None:
        """`loc min <s>`: the device's own minimum gap between on-demand fix
        *attempts* (PROTOCOL.md §13.3 rule 1), default 120s -- reported in
        `/status` as `loc_min_s`."""
        self._loc_min_s = seconds

    def loc_set_fail(self, on: bool) -> None:
        """`loc fail on|off`: simulate no-fix -- every attempt while this is
        on answers `err:"no_fix"` instead of a real fix (§13.2)."""
        self._loc_fail = on

    def _handle_loc_req(self, req_id: str | None) -> None:
        """PROTOCOL.md §13.3 rules 1-3, normative: answers a `loc_req` down
        message on `/loc`, subject to the device's own rate limit."""
        if not req_id:
            return
        with self._loc_lock:
            now = time.time()
            if (
                self._last_attempt_ts is not None
                and (now - self._last_attempt_ts) < self._loc_min_s
                and self._last_fix is not None
            ):
                # Rule 1: less than loc_min_s since the last *attempt* ->
                # answer immediately from the last fix, cached:true, without
                # powering GNSS. (If there is no last fix at all yet -- this
                # device has never obtained one -- there is nothing to
                # answer from, so fall through to a real attempt instead;
                # PROTOCOL.md does not address this bootstrap case
                # explicitly.)
                fix = self._last_fix
                cached = True
            else:
                # Rule 2: attempt a fix (this simulator's attempts are
                # instantaneous; a real device bounds this at 60s). Rule 3:
                # the lock above already serialises concurrent attempts, so
                # every `loc_req` processed here reflects the one attempt's
                # result.
                fix = self._take_fix()
                self._last_attempt_ts = now
                cached = False

        def _answer(delay_s: float) -> None:
            if delay_s > 0:
                time.sleep(delay_s)
            if fix is None:
                self.publish_loc(req=req_id, loc=None, cached=False, err="no_fix")
            else:
                self.publish_loc(req=req_id, loc=fix, cached=cached)

        # `loc_answer_delay_s` only stands in for real GNSS acquisition time
        # (rule 2's "attempt a fix"); the cached path (rule 1) never powers
        # GNSS at all and always answers immediately, delay or not.
        delay_s = self.loc_answer_delay_s if not cached else 0.0
        if delay_s > 0:
            threading.Thread(target=_answer, args=(delay_s,), daemon=True).start()
        else:
            _answer(0.0)


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

    def sweep(self) -> dict[str, Any]:
        """`POST /internal/sweep` -- docs/SERVER_PLAN.md §5.7's retention
        sweep, `sweep` command per §8."""
        resp = self.api_post("/internal/sweep", {})
        resp.raise_for_status()
        return resp.json()

    def add_backend(self, kind: str, config: dict[str, Any], *, enabled: bool = True) -> dict[str, Any]:
        """`POST /api/me/backends` for the currently-logged-in user --
        docs/SERVER_PLAN.md §5.1. Used by `backend add` and by
        `tools/e2e_v2.py`'s `fanout` scenario to give a user an `sms`
        backend (`app/backends/sms_twilio.py`) without any admin
        involvement, matching a real user configuring their own backends
        (§7.4).

        Note: for `kind='sms'`/`'gchat'` the
        relay always creates the row `enabled=False` regardless of the
        `enabled` argument here -- a link/verify-flow backend is not
        trusted to receive real traffic until `verify_backend()` below
        succeeds. Callers that need an immediately-usable sms backend (this
        script's `fanout` scenario) must complete that flow themselves."""
        resp = self.api_post("/api/me/backends", {"kind": kind, "config": config, "enabled": enabled})
        if resp.status_code >= 400:
            raise RuntimeError(f"add_backend failed: {resp.status_code} {resp.text}")
        return resp.json()

    def verify_backend(self, bid: str, code: str) -> dict[str, Any]:
        """`POST /api/me/backends/{id}/verify` for the currently-logged-in
        user -- completes the link/verify flow `add_backend` above starts
        for `sms`/`gchat` kinds (docs/SERVER_PLAN.md §5.1)."""
        resp = self.api_post(f"/api/me/backends/{bid}/verify", {"code": code})
        if resp.status_code >= 400:
            raise RuntimeError(f"verify_backend failed: {resp.status_code} {resp.text}")
        return resp.json()

    def locate(self, alias: str) -> dict[str, Any]:
        resp = self.api_post(f"/api/conversations/{alias}/locate", {})
        if resp.status_code >= 400:
            raise RuntimeError(f"locate failed: {resp.status_code} {resp.text}")
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

    def admin_set_retention(
        self, *, messages: str | None = None, locations: str | None = None
    ) -> dict[str, Any]:
        """`admin settings retention messages=<n><d|w> locations=<n><d|w>`
        (docs/SERVER_PLAN.md §8) -- `PUT /api/admin/settings` replaces both
        classes at once (§5.1), so this reads the current values back first
        (`GET /api/admin/settings`) and only overrides the class(es) given,
        the same "read-then-selectively-override" shape
        `admin_set_allow`/`admin_deny` use for the allowlist."""
        current = self.api_get("/api/admin/settings").json()
        payload = {"messages": current["messages"], "locations": current["locations"]}
        if messages is not None:
            payload["messages"] = parse_retention_shorthand(messages)
        if locations is not None:
            payload["locations"] = parse_retention_shorthand(locations)
        resp = self.api_put("/api/admin/settings", payload)
        if resp.status_code >= 400:
            raise RuntimeError(f"admin settings retention failed: {resp.status_code} {resp.text}")
        return resp.json()

    # ---- admin: address book (docs/DEVICE_TASKS.md S4.1/S4.2/S4.5,
    # docs/DEVICE_PLAN.md §4.3, §5.8) ----

    def admin_list_contacts(
        self, status: Literal["pending", "approved", "rejected"] | None = None
    ) -> list[dict[str, Any]]:
        path = "/api/admin/contacts" + (f"?status={status}" if status else "")
        resp = self.api_get(path)
        resp.raise_for_status()
        return resp.json()

    def admin_approve_contact(
        self,
        key: str,
        *,
        mode: Literal["link", "create"],
        alias: str | None = None,
        locate: bool = False,
    ) -> dict[str, Any]:
        """`POST /api/admin/contacts/{key}/approve` -- docs/DEVICE_PLAN.md
        §4.3's approval dialog. `key` is the `contactRequests/{deviceId}_
        {reqId}` document id, as returned by `admin_list_contacts()`."""
        body: dict[str, Any] = {"mode": mode, "locate": locate}
        if alias is not None:
            body["alias"] = alias
        resp = self.api_post(f"/api/admin/contacts/{key}/approve", body)
        if resp.status_code >= 400:
            raise RuntimeError(f"admin approve_contact failed: {resp.status_code} {resp.text}")
        return resp.json()

    def admin_reject_contact(self, key: str, reason: str) -> dict[str, Any]:
        resp = self.api_post(f"/api/admin/contacts/{key}/reject", {"reason": reason})
        if resp.status_code >= 400:
            raise RuntimeError(f"admin reject_contact failed: {resp.status_code} {resp.text}")
        return resp.json()

    def admin_push_cfg(
        self, device_id: str, *, auto: int | None = None, clear: bool | None = None
    ) -> dict[str, Any]:
        """`POST /api/admin/devices/{id}/cfg` -- docs/DEVICE_PLAN.md §5.8's
        remote lock controls (`cfg.lock.auto`/`cfg.lock.clear`)."""
        lock: dict[str, Any] = {}
        if auto is not None:
            lock["auto"] = auto
        if clear is not None:
            lock["clear"] = clear
        resp = self.api_post(f"/api/admin/devices/{device_id}/cfg", {"lock": lock})
        if resp.status_code >= 400:
            raise RuntimeError(f"admin push_cfg failed: {resp.status_code} {resp.text}")
        return resp.json()

    # ---- Firestore REST reads (exercises firestore.rules) ----

    def firestore_get(self, path: str) -> httpx.Response:
        return self._http.get(f"{self.firestore_base}/{path}", headers=self._headers())

    def firestore_run_query(self, body: dict[str, Any]) -> httpx.Response:
        return self._http.post(
            f"{self.firestore_base}:runQuery", json=body, headers=self._headers()
        )

    def firestore_run_query_at(self, parent_path: str, body: dict[str, Any]) -> httpx.Response:
        """Same as `firestore_run_query`, but scoped to a subcollection
        query rooted at `parent_path` (e.g. `devices/{deviceId}`) instead of
        the database root -- what `locations()` needs to query
        `devices/{deviceId}/locations`."""
        return self._http.post(
            f"{self.firestore_base}/{parent_path}:runQuery", json=body, headers=self._headers()
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
                # Both filters, not just `convKey`. A Firestore `list` is
                # authorised by an abstract pre-check against the query's
                # declared filters, before any document is read, so
                # `convKey` alone gives `firestore.rules`' messages rule
                # (`uid in resource.data.uids`) nothing to prove itself from
                # and the whole query 403s. `uids array-contains <self>` is
                # redundant against `convKey` but is what makes it provable
                # -- same fix as the web app's thread listener
                # (web/app/chat/[alias]/ThreadPageClient.tsx).
                "where": {
                    "compositeFilter": {
                        "op": "AND",
                        "filters": [
                            {
                                "fieldFilter": {
                                    "field": {"fieldPath": "convKey"},
                                    "op": "EQUAL",
                                    "value": {"stringValue": conv_key},
                                }
                            },
                            {
                                "fieldFilter": {
                                    "field": {"fieldPath": "uids"},
                                    "op": "ARRAY_CONTAINS",
                                    "value": {"stringValue": self.uid},
                                }
                            },
                        ],
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

    def _device_id_for_owner(self, owner_uid: str) -> str | None:
        """Resolves a device id owned by `owner_uid` that *this caller* may
        locate -- exercises `firestore.rules`' `devices/{d}` `locatableBy`
        check the same way `locations()` below exercises the `locations`
        subcollection's. Deliberately filters on `locatableBy array_contains
        self.uid` rather than `ownerUid == owner_uid`:
        Firestore evaluates a security rule
        for a `list` (collection query) request *abstractly*, against the
        query's own declared filters alone, before ever touching a real
        document -- a query filtered only on `ownerUid` gives the rule
        nothing to statically prove the `locatableBy`/`isAdmin()` disjuncts
        from, so Firestore denies the *entire query* with `403` even for a
        caller who is genuinely allowed to read the one matching document
        (this is documented, intentional Firestore behaviour for `list`,
        not a bug in `firestore.rules`).
        Filtering on `locatableBy array_contains self.uid` instead gives
        Firestore exactly the fact its rule needs to prove the query safe,
        so it *can* stream real results -- which may include devices owned
        by users other than `owner_uid` (every device this caller may
        locate at all), so the match on `owner_uid` happens client-side
        below rather than in the query."""
        query = {
            "structuredQuery": {
                "from": [{"collectionId": "devices"}],
                "where": {
                    "fieldFilter": {
                        "field": {"fieldPath": "locatableBy"},
                        "op": "ARRAY_CONTAINS",
                        "value": {"stringValue": self.uid},
                    }
                },
            }
        }
        resp = self.firestore_run_query(query)
        resp.raise_for_status()
        for row in resp.json():
            doc = row.get("document")
            if not doc:
                continue
            fields = doc["fields"]
            if fields.get("ownerUid", {}).get("stringValue") == owner_uid:
                return doc["name"].rsplit("/", 1)[-1]
        return None

    def locations(self, alias: str, n: int = 20) -> list[dict[str, Any]]:
        """`locations <alias>`: the target's `n` most recent
        `devices/{deviceId}/locations` fixes, newest first -- read straight
        from Firestore (no relay API endpoint for this, per
        docs/SERVER_PLAN.md §5.1: "reads the web app can do straight from
        Firestore ... have no API endpoint"), which is what exercises
        `firestore.rules`' `locatableBy` check: a caller without `locate`
        permission either can't resolve a device id at all (see
        `_device_id_for_owner`) or, if it already knows one, gets a 403 on
        the subcollection query."""
        peer_uid = self._resolve_alias_best_effort(alias)
        device_id = self._device_id_for_owner(peer_uid)
        if device_id is None:
            raise RuntimeError(
                f"no device found for {alias!r} (either it has none, or this caller is not "
                "permitted to see it -- firestore.rules filtered it out)"
            )
        query = {
            "structuredQuery": {
                "from": [{"collectionId": "locations"}],
                "orderBy": [{"field": {"fieldPath": "createdAt"}, "direction": "DESCENDING"}],
                "limit": n,
            }
        }
        resp = self.firestore_run_query_at(f"devices/{device_id}", query)
        if resp.status_code == 403:
            raise RuntimeError(f"locations({alias!r}) denied by firestore.rules (403)")
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
                    "lat": _fs_value(fields.get("lat")),
                    "lon": _fs_value(fields.get("lon")),
                    "accM": _fs_value(fields.get("accM")),
                    "fixTs": _fs_value(fields.get("fixTs")),
                    "cached": _fs_value(fields.get("cached")),
                }
            )
        return out


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

    def do_loc(self, arg: str) -> None:
        parts = shlex.split(arg)
        if not parts:
            print(
                "usage: loc <lat> <lon> [acc] | loc auto <period_s> [--walk] | "
                "loc min <s> | loc fail on|off"
            )
            return
        if parts[0] == "auto":
            parser = argparse.ArgumentParser(prog="loc auto", add_help=False)
            parser.add_argument("period_s", type=int)
            parser.add_argument("--walk", action="store_true")
            try:
                ns = parser.parse_args(parts[1:])
            except SystemExit:
                return
            self.device.loc_auto(ns.period_s, ns.walk)
            print(f"loc auto: period={ns.period_s}s walk={ns.walk}")
        elif parts[0] == "min":
            if len(parts) != 2:
                print("usage: loc min <s>")
                return
            self.device.loc_set_min(int(parts[1]))
            print(f"loc min = {parts[1]}s")
        elif parts[0] == "fail":
            if len(parts) != 2 or parts[1] not in ("on", "off"):
                print("usage: loc fail on|off")
                return
            self.device.loc_set_fail(parts[1] == "on")
            print(f"loc fail = {parts[1]}")
        else:
            try:
                lat = float(parts[0])
                lon = float(parts[1])
                acc = int(parts[2]) if len(parts) > 2 else None
            except (ValueError, IndexError):
                print("usage: loc <lat> <lon> [acc]")
                return
            self.device.loc_now(lat, lon, acc)

    def do_contactreq(self, arg: str) -> None:
        parts = shlex.split(arg)
        if not parts:
            print("usage: contactreq <name> [phone-or-alias]")
            return
        name = parts[0]
        ph = parts[1] if len(parts) > 1 else None
        self.device.publish_contact_req(name, ph)

    def do_book(self, arg: str) -> None:
        """Shows this device's last-applied address book (`None` before one
        has ever arrived) -- docs/PROTOCOL.md §3.2 `kind:"book"`."""
        self._out(self.device.book)

    def do_lock(self, arg: str) -> None:
        """Shows this device's applied `cfg.lock` state -- docs/DEVICE_PLAN.md
        §5.8, docs/PROTOCOL.md §3.2 `kind:"cfg"`."""
        self._out(
            {"auto_min": self.device.lock_auto_min, "cleared_count": self.device.lock_cleared_count}
        )

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

    def do_locate(self, arg: str) -> None:
        alias = arg.strip()
        if not alias:
            print("usage: locate <alias>")
            return
        self._out(self.server.locate(alias))

    def do_locations(self, arg: str) -> None:
        parts = arg.split()
        if not parts:
            print("usage: locations <alias> [n]")
            return
        alias = parts[0]
        n = int(parts[1]) if len(parts) > 1 else 20
        self._out(self.server.locations(alias, n))

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

    def do_sweep(self, arg: str) -> None:
        self._out(self.server.sweep())

    def do_backend(self, arg: str) -> None:
        parts = shlex.split(arg)
        if len(parts) < 1 or parts[0] != "add":
            print("usage: backend add <kind> <json-config> [--disabled]")
            return
        self._backend_add(parts[1:])

    def _backend_add(self, args: list[str]) -> None:
        parser = argparse.ArgumentParser(prog="backend add", add_help=False)
        parser.add_argument("kind")
        parser.add_argument("config", help="JSON object, e.g. '{\"phone\": \"+15551234567\"}'")
        parser.add_argument("--disabled", action="store_true")
        try:
            ns = parser.parse_args(args)
        except SystemExit:
            return
        try:
            config = json.loads(ns.config)
        except json.JSONDecodeError as exc:
            print(f"invalid JSON config: {exc}")
            return
        self._out(self.server.add_backend(ns.kind, config, enabled=not ns.disabled))

    def do_admin(self, arg: str) -> None:
        parts = shlex.split(arg)
        if not parts:
            print(
                "usage: admin user-add|device-add|allow|deny|settings|contacts|approve|reject|cfg ..."
            )
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
        elif sub == "settings":
            self._admin_settings(rest)
        elif sub == "contacts":
            self._admin_contacts(rest)
        elif sub == "approve":
            self._admin_approve(rest)
        elif sub == "reject":
            self._admin_reject(rest)
        elif sub == "cfg":
            self._admin_cfg(rest)
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

    def _admin_settings(self, args: list[str]) -> None:
        if not args or args[0] != "retention":
            print("usage: admin settings retention messages=<n><d|w> locations=<n><d|w>")
            return
        kv: dict[str, str] = {}
        for item in args[1:]:
            key, sep, value = item.partition("=")
            if not sep:
                print("usage: admin settings retention messages=<n><d|w> locations=<n><d|w>")
                return
            kv[key] = value
        try:
            result = self.server.admin_set_retention(
                messages=kv.get("messages"), locations=kv.get("locations")
            )
        except ValueError as exc:
            print(f"invalid retention value: {exc}")
            return
        self._out(result)

    def _admin_contacts(self, args: list[str]) -> None:
        status = args[0] if args else None
        if status is not None and status not in ("pending", "approved", "rejected"):
            print("usage: admin contacts [pending|approved|rejected]")
            return
        self._out(self.server.admin_list_contacts(status))

    def _admin_approve(self, args: list[str]) -> None:
        parser = argparse.ArgumentParser(prog="admin approve", add_help=False)
        parser.add_argument("key")
        parser.add_argument("mode", choices=["link", "create"])
        parser.add_argument("alias", nargs="?")
        parser.add_argument("--locate", action="store_true")
        try:
            ns = parser.parse_args(args)
        except SystemExit:
            return
        self._out(
            self.server.admin_approve_contact(
                ns.key, mode=ns.mode, alias=ns.alias, locate=ns.locate
            )
        )

    def _admin_reject(self, args: list[str]) -> None:
        if len(args) < 2:
            print("usage: admin reject <key> <reason>")
            return
        self._out(self.server.admin_reject_contact(args[0], " ".join(args[1:])))

    def _admin_cfg(self, args: list[str]) -> None:
        parser = argparse.ArgumentParser(prog="admin cfg", add_help=False)
        parser.add_argument("device_id")
        parser.add_argument("--auto", type=int)
        parser.add_argument("--clear", action="store_true")
        try:
            ns = parser.parse_args(args)
        except SystemExit:
            return
        self._out(
            self.server.admin_push_cfg(
                ns.device_id, auto=ns.auto, clear=ns.clear if ns.clear else None
            )
        )

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
    parser.add_argument(
        "--hmac-key",
        default=os.environ.get("PAGER_HMAC_KEY", ""),
        help="base64 device HMAC key (docs/PROTOCOL.md §14) -- every publish is signed "
        "with it and every /down is verified and dropped on failure. Omit or pass '' to "
        "play an unsigned authMode:password device (the default).",
    )
    parser.add_argument(
        "--wire",
        choices=["json", "cbor"],
        default=os.environ.get("PAGER_WIRE", "json"),
        help="wire encoding for this device's publishes (default: json, "
        "docs/DEVICE_PLAN.md §2.4's default for tools/pager_client.py)",
    )
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

    hmac_key = base64.b64decode(args.hmac_key) if args.hmac_key else None
    device = DeviceClient(
        args.device_id,
        args.host,
        args.port,
        args.username,
        args.password,
        hmac_key=hmac_key,
        wire=args.wire,
    )
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

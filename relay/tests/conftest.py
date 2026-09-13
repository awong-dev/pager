from __future__ import annotations

import json
import time
from collections.abc import Iterator

import pytest

from app.mqtt_gateway import MqttGateway
from app.store import Store
from tests.fake_transport import FakeTransport


@pytest.fixture
def store(tmp_path) -> Iterator[Store]:
    s = Store(str(tmp_path / "relay.db"))
    yield s
    s.close()


@pytest.fixture
def transport() -> FakeTransport:
    return FakeTransport()


@pytest.fixture
def gateway(store: Store, transport: FakeTransport) -> MqttGateway:
    gw = MqttGateway(store, transport)
    gw.start()
    return gw


def up_topic(device_id: str) -> str:
    return f"pager/{device_id}/up"


def status_topic(device_id: str) -> str:
    return f"pager/{device_id}/status"


def down_topic(device_id: str) -> str:
    return f"pager/{device_id}/down"


def encode(obj: dict) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode("utf-8")


def ack_payload(msg_id: str, ack: str, ts: int | None = None) -> bytes:
    ts = ts if ts is not None else int(time.time())
    return encode({"v": 1, "id": msg_id, "ts": ts, "ack": ack})


def up_message_payload(
    msg_id: str, body: str, *, sender: str = "student", ts: int | None = None
) -> bytes:
    ts = ts if ts is not None else int(time.time())
    return encode({"v": 1, "id": msg_id, "ts": ts, "from": sender, "body": body, "ack": None})


def online_status_payload(session: str, **overrides) -> bytes:
    payload = {
        "v": 1,
        "state": "online",
        "mode": "sleep",
        "batt_mv": 3280,
        "rssi": -95,
        "session": session,
        "ts": int(time.time()),
        "fw": "0.1.0",
    }
    payload.update(overrides)
    return encode(payload)


def offline_status_payload(session: str) -> bytes:
    return encode({"v": 1, "state": "offline", "session": session})

from __future__ import annotations

import json
import time
from collections.abc import Iterator

import pytest

from app.ingest import Ingest
from app.store import Store
from tests.fake_transport import FakeBrokerClient


@pytest.fixture
def store(tmp_path) -> Iterator[Store]:
    s = Store(str(tmp_path / "relay.db"))
    yield s
    s.close()


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def ingest(store: Store, broker: FakeBrokerClient) -> Ingest:
    return Ingest(store, broker)


def up_topic(device_id: str) -> str:
    return f"pager/{device_id}/up"


def status_topic(device_id: str) -> str:
    return f"pager/{device_id}/status"


def down_topic(device_id: str) -> str:
    return f"pager/{device_id}/down"


def loc_topic(device_id: str) -> str:
    return f"pager/{device_id}/loc"


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


def loc_payload(
    loc_id: str, *, lat: float = 37.7749, lon: float = -122.4194, req: str | None = None
) -> bytes:
    return encode(
        {
            "v": 1,
            "id": loc_id,
            "ts": int(time.time()),
            "loc": {"lat": lat, "lon": lon, "fix_ts": int(time.time())},
            "req": req,
        }
    )

#!/usr/bin/env python3
"""
Fake device for local testing — see HANDOFF.md Phase 3 and docs/PROTOCOL.md.

Connects to the broker as a device (paho-mqtt), mirroring the wire contract:
  - sets an LWT on pager/{device_id}/status (§5.2 shape)
  - publishes an `online` status on connect (§5.1 shape)
  - subscribes to pager/{device_id}/down QoS 1 (the device's only subscription)
  - prints every down message it receives
  - immediately auto-acks `shown` for each one
  - with --reply, also publishes one student up-message after connecting

This is a reference/testing tool, not firmware: it uses a normal ~60s MQTT
keepalive rather than the device's real 1800s (PROTOCOL.md §6.2), since a
laptop process has no battery budget to protect.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import time
from types import FrameType

import paho.mqtt.client as mqtt

SIM_KEEPALIVE_S = 60


def new_id(prefix: str) -> str:
    """`prefix` + 8 lowercase hex chars, per docs/PROTOCOL.md §1."""
    return f"{prefix}{os.urandom(4).hex()}"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Simulated pager device")
    parser.add_argument("--device-id", required=True, help="Device ID")
    parser.add_argument("--reply", help="Auto-reply message text, sent once after connecting")
    parser.add_argument("--host", default=os.environ.get("MQTT_BROKER_HOST", "localhost"))
    parser.add_argument(
        "--port", type=int, default=int(os.environ.get("MQTT_BROKER_PORT", "1883"))
    )
    parser.add_argument("--username", default=os.environ.get("MQTT_USERNAME"))
    parser.add_argument("--password", default=os.environ.get("MQTT_PASSWORD"))
    # There is no real modem/battery in the simulator; these are reasonable
    # stand-ins for the §5.1 online-status fields.
    parser.add_argument("--mode", default="sleep", choices=["sleep", "active"])
    parser.add_argument("--batt-mv", type=int, default=3280)
    parser.add_argument("--rssi", type=int, default=-85)
    parser.add_argument("--fw", default="sim-0.1.0")
    return parser


def main() -> None:
    args = build_arg_parser().parse_args()
    device_id = args.device_id
    session_id = new_id("s_")

    down_topic = f"pager/{device_id}/down"
    up_topic = f"pager/{device_id}/up"
    status_topic = f"pager/{device_id}/status"

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=device_id,
        clean_session=False,
        protocol=mqtt.MQTTv311,
    )
    if args.username:
        client.username_pw_set(args.username, args.password)

    lwt_payload = json.dumps(
        {"v": 1, "state": "offline", "session": session_id}, separators=(",", ":")
    )
    client.will_set(status_topic, lwt_payload, qos=1, retain=True)

    def publish_online_status() -> None:
        payload = json.dumps(
            {
                "v": 1,
                "state": "online",
                "mode": args.mode,
                "batt_mv": args.batt_mv,
                "rssi": args.rssi,
                "session": session_id,
                "ts": int(time.time()),
                "fw": args.fw,
            },
            separators=(",", ":"),
        )
        client.publish(status_topic, payload, qos=1, retain=True)

    def publish_ack(msg_id: str, ack: str) -> None:
        payload = json.dumps(
            {"v": 1, "id": msg_id, "ts": int(time.time()), "ack": ack}, separators=(",", ":")
        )
        client.publish(up_topic, payload, qos=1)
        print(f"-> acked {ack} for {msg_id}")

    def publish_reply(body: str) -> None:
        payload = json.dumps(
            {
                "v": 1,
                "id": new_id("u_"),
                "ts": int(time.time()),
                "from": "student",
                "body": body,
                "ack": None,
            },
            separators=(",", ":"),
        )
        client.publish(up_topic, payload, qos=1)
        print(f"-> sent reply: {body!r}")

    def on_connect(client, userdata, connect_flags, reason_code, properties) -> None:
        print(f"connected to {args.host}:{args.port} as {device_id} (session={session_id})")
        client.subscribe(down_topic, qos=1)
        publish_online_status()
        if args.reply:
            publish_reply(args.reply)

    def on_message(client, userdata, msg) -> None:
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            print(f"<- received unparsable payload on {msg.topic}: {msg.payload[:64]!r}")
            return
        print(f"<- {msg.topic}: {data}")
        msg_id = data.get("id")
        if msg_id:
            publish_ack(msg_id, "shown")

    def on_disconnect(client, userdata, disconnect_flags, reason_code, properties) -> None:
        print(f"disconnected (reason={reason_code})")

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    client.connect(args.host, args.port, keepalive=SIM_KEEPALIVE_S)

    def shutdown(signum: int, frame: FrameType | None) -> None:
        print("shutting down")
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    client.loop_forever()


if __name__ == "__main__":
    main()

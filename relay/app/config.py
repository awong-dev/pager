"""Runtime configuration loaded strictly from environment variables.

See relay/.env.example for the documented set of variables. Nothing here has
a real secret default -- RELAY_TOKEN defaults to "" so a misconfigured
deployment fails closed (every request gets 401) rather than open.
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Settings:
    relay_token: str
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str | None
    mqtt_password: str | None
    db_path: str

    @classmethod
    def from_env(cls) -> Settings:
        return cls(
            relay_token=os.environ.get("RELAY_TOKEN", ""),
            mqtt_host=os.environ.get("MQTT_BROKER_HOST", "localhost"),
            mqtt_port=int(os.environ.get("MQTT_BROKER_PORT", "1883")),
            mqtt_username=os.environ.get("MQTT_USERNAME") or None,
            mqtt_password=os.environ.get("MQTT_PASSWORD") or None,
            db_path=os.environ.get("RELAY_DB_PATH", "relay.db"),
        )

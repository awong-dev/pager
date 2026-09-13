"""SQLite-backed message and status store.

Schema lives in numbered .sql files under app/db/migrations/, applied at
startup (see _apply_migrations). One sqlite3 connection is shared between
the FastAPI request threads and the MQTT background thread; every public
method takes a lock around its DB work since sqlite3 connections are not
safe for concurrent use across threads even with check_same_thread=False.
"""

from __future__ import annotations

import sqlite3
import threading
import time
from dataclasses import dataclass
from pathlib import Path

MIGRATIONS_DIR = Path(__file__).parent / "db" / "migrations"

EXPIRY_SECONDS = 24 * 60 * 60
REPUBLISH_CAP = 10

AckResult = str  # "updated" | "noop" | "unknown"


@dataclass(frozen=True, slots=True)
class MessageRow:
    id: str
    device_id: str
    direction: str
    v: int
    ts: int
    sender: str | None
    body: str | None
    state: str
    shown_ts: int | None
    read_ts: int | None
    created_at: int
    seq: int

    def effective_state(self, now: int | None = None) -> str:
        """§4.1: `expired` is a terminal state computed lazily (never
        persisted) for a down message still queued/sent 24h after creation."""
        if self.direction == "up":
            return "received"
        now = now if now is not None else int(time.time())
        if self.state in ("queued", "sent") and (now - self.created_at) >= EXPIRY_SECONDS:
            return "expired"
        return self.state


@dataclass(frozen=True, slots=True)
class StatusRow:
    device_id: str
    state: str
    mode: str | None
    batt_mv: int | None
    rssi: int | None
    session: str | None
    ts: int | None
    fw: str | None
    updated_at: int


class Store:
    def __init__(self, db_path: str) -> None:
        self._lock = threading.RLock()
        self._conn = sqlite3.connect(db_path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA foreign_keys = ON")
        self._apply_migrations()

    def _apply_migrations(self) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                "CREATE TABLE IF NOT EXISTS schema_migrations (filename TEXT PRIMARY KEY)"
            )
            applied = {
                row[0] for row in self._conn.execute("SELECT filename FROM schema_migrations")
            }
            for path in sorted(MIGRATIONS_DIR.glob("*.sql")):
                if path.name in applied:
                    continue
                self._conn.executescript(path.read_text())
                self._conn.execute(
                    "INSERT INTO schema_migrations (filename) VALUES (?)", (path.name,)
                )

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    # ---- messages: writes ----

    def create_down_message(
        self, *, msg_id: str, device_id: str, ts: int, body: str, now: int | None = None
    ) -> MessageRow:
        now = now if now is not None else int(time.time())
        with self._lock, self._conn:
            self._conn.execute(
                """
                INSERT INTO messages
                    (id, device_id, direction, v, ts, sender, body, state, created_at)
                VALUES (?, ?, 'down', 1, ?, 'parent', ?, 'queued', ?)
                """,
                (msg_id, device_id, ts, body, now),
            )
        row = self.get_message(msg_id)
        assert row is not None
        return row

    def insert_up_message(
        self,
        *,
        msg_id: str,
        device_id: str,
        ts: int,
        sender: str | None,
        body: str | None,
        now: int | None = None,
    ) -> None:
        now = now if now is not None else int(time.time())
        with self._lock, self._conn:
            self._conn.execute(
                """
                INSERT INTO messages
                    (id, device_id, direction, v, ts, sender, body, state, created_at)
                VALUES (?, ?, 'up', 1, ?, ?, ?, 'received', ?)
                """,
                (msg_id, device_id, ts, sender, body, now),
            )

    def mark_sent(self, msg_id: str) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                "UPDATE messages SET state = 'sent' WHERE id = ? AND state = 'queued'",
                (msg_id,),
            )

    def apply_ack(self, msg_id: str, ack: str, ack_ts: int) -> AckResult:
        """Apply the down-message ack state machine (§4.1). Caller is
        responsible for the unknown-id and wrong-device checks -- this only
        implements the idempotent / out-of-order monotonic transition."""
        with self._lock, self._conn:
            row = self._conn.execute(
                "SELECT state, shown_ts, read_ts FROM messages WHERE id = ?", (msg_id,)
            ).fetchone()
            if row is None:
                return "unknown"
            state = row["state"]
            if ack == "shown":
                if state in ("shown", "read"):
                    return "noop"
                self._conn.execute(
                    "UPDATE messages SET state = 'shown', shown_ts = ? WHERE id = ?",
                    (ack_ts, msg_id),
                )
                return "updated"
            if ack == "read":
                if state == "read":
                    return "noop"
                backfilled_shown_ts = row["shown_ts"] if row["shown_ts"] is not None else ack_ts
                self._conn.execute(
                    "UPDATE messages SET state = 'read', read_ts = ?, shown_ts = ? WHERE id = ?",
                    (ack_ts, backfilled_shown_ts, msg_id),
                )
                return "updated"
            return "unknown"

    # ---- messages: reads ----

    def id_exists(self, msg_id: str) -> bool:
        with self._lock:
            row = self._conn.execute("SELECT 1 FROM messages WHERE id = ?", (msg_id,)).fetchone()
        return row is not None

    def get_message(self, msg_id: str) -> MessageRow | None:
        with self._lock:
            row = self._conn.execute("SELECT * FROM messages WHERE id = ?", (msg_id,)).fetchone()
        return self._row_to_message(row) if row else None

    def get_thread(self, device_id: str, since: int | None = None) -> list[MessageRow]:
        with self._lock:
            if since is None:
                rows = self._conn.execute(
                    "SELECT * FROM messages WHERE device_id = ? ORDER BY seq ASC",
                    (device_id,),
                ).fetchall()
            else:
                rows = self._conn.execute(
                    "SELECT * FROM messages WHERE device_id = ? AND created_at > ?"
                    " ORDER BY seq ASC",
                    (device_id, since),
                ).fetchall()
        return [self._row_to_message(r) for r in rows]

    def get_republish_candidates(
        self,
        device_id: str,
        *,
        now: int | None = None,
        max_age: int = EXPIRY_SECONDS,
        limit: int = REPUBLISH_CAP,
    ) -> list[MessageRow]:
        """§5.3: oldest first, state in (queued, sent), age < 24h, capped."""
        now = now if now is not None else int(time.time())
        cutoff = now - max_age
        with self._lock:
            rows = self._conn.execute(
                """
                SELECT * FROM messages
                WHERE device_id = ? AND direction = 'down' AND state IN ('queued', 'sent')
                  AND created_at > ?
                ORDER BY seq ASC
                LIMIT ?
                """,
                (device_id, cutoff, limit),
            ).fetchall()
        return [self._row_to_message(r) for r in rows]

    # ---- status ----

    def get_status(self, device_id: str) -> StatusRow | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT * FROM device_status WHERE device_id = ?", (device_id,)
            ).fetchone()
        return self._row_to_status(row) if row else None

    def upsert_status(
        self,
        device_id: str,
        *,
        state: str,
        mode: str | None,
        batt_mv: int | None,
        rssi: int | None,
        session: str | None,
        ts: int | None,
        fw: str | None,
        now: int | None = None,
    ) -> None:
        now = now if now is not None else int(time.time())
        with self._lock, self._conn:
            self._conn.execute(
                """
                INSERT INTO device_status
                    (device_id, state, mode, batt_mv, rssi, session, ts, fw, updated_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    state = excluded.state,
                    mode = excluded.mode,
                    batt_mv = excluded.batt_mv,
                    rssi = excluded.rssi,
                    session = excluded.session,
                    ts = excluded.ts,
                    fw = excluded.fw,
                    updated_at = excluded.updated_at
                """,
                (device_id, state, mode, batt_mv, rssi, session, ts, fw, now),
            )

    # ---- row mapping ----

    @staticmethod
    def _row_to_message(row: sqlite3.Row) -> MessageRow:
        return MessageRow(
            id=row["id"],
            device_id=row["device_id"],
            direction=row["direction"],
            v=row["v"],
            ts=row["ts"],
            sender=row["sender"],
            body=row["body"],
            state=row["state"],
            shown_ts=row["shown_ts"],
            read_ts=row["read_ts"],
            created_at=row["created_at"],
            seq=row["seq"],
        )

    @staticmethod
    def _row_to_status(row: sqlite3.Row) -> StatusRow:
        return StatusRow(
            device_id=row["device_id"],
            state=row["state"],
            mode=row["mode"],
            batt_mv=row["batt_mv"],
            rssi=row["rssi"],
            session=row["session"],
            ts=row["ts"],
            fw=row["fw"],
            updated_at=row["updated_at"],
        )

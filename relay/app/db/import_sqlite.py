"""`python -m app.db.import_sqlite <path-to-relay.db>` -- docs/SERVER_PLAN.md
§3 "MVP import", §10 Phase 2.

One-off migration of the MVP's SQLite store (`app/db/migrations/0001_init.sql`
-- a `messages` table with `direction in ('down','up')` and a
`device_status` table, both keyed on `device_id`) into the v2 Firestore
model:

- a `parent` user and a `student` user (the MVP's only two "identities" --
  every down message was from `parent`, every up message from `student`);
- one `webapp` backend for each, since every user gets an implicit `webapp`
  backend at creation (docs/SERVER_PLAN.md §6.3);
- an `allow` edge in both directions (`message` and `locate` both true) so
  the migrated thread is immediately usable under the new allow-list model;
- one `devices/{deviceId}` document per distinct `device_id` found in the
  old DB, owned by `student`, `defaultToUid` = the parent's uid (matching
  the MVP's single-recipient behaviour) -- `mqttPasswordHash` is set to a
  sentinel (`"imported-unknown"`), because the plaintext password was never
  stored anywhere this script can read; a real deployment must rotate
  credentials for any imported device before using it
  (`POST /api/admin/devices/{id}/rotate-credentials`);
- one `messages/{id}` document per old message row, preserving the
  original `id` and insertion order (`seq`), with its ack-state history
  folded into `deliveries.pager` for down messages (`state` is the row's
  *effective* state -- `expired` is computed at import time exactly like
  the MVP computed it lazily at read time, docs/PROTOCOL.md §4.1).

Idempotent-ish: re-running against the same SQLite file re-creates the
same users/devices (Firestore `create()`/`set()` calls will simply upsert or
raise `AliasTaken`/`AlreadyExists` for anything already migrated) -- intended
for a single one-off run per deployment, not a sync tool.
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from datetime import UTC, datetime

from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store

PARENT_ALIAS = "parent"
STUDENT_ALIAS = "student"
EXPIRY_SECONDS = 24 * 60 * 60


def _effective_state(row: sqlite3.Row, now: int) -> str:
    state = row["state"]
    if state in ("queued", "sent") and (now - row["created_at"]) >= EXPIRY_SECONDS:
        return "expired"
    return state


def _ensure_user(alias: str, display_name: str) -> str:
    existing = users_store.get_user_by_alias(alias)
    if existing is not None:
        return existing.uid
    uid = f"imported-{alias}"
    user = users_store.create_user(uid=uid, alias=alias, display_name=display_name)
    return user.uid


def _ensure_webapp_backend(uid: str) -> None:
    if not any(b.kind == "webapp" for b in backends_store.list_backends(uid)):
        backends_store.create_backend(uid, kind="webapp", config={}, enabled=True)


def import_sqlite(db_path: str, *, now: int | None = None) -> dict[str, int]:
    import time

    now = now if now is not None else int(time.time())

    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    try:
        message_rows = conn.execute("SELECT * FROM messages ORDER BY seq ASC").fetchall()
        status_rows = conn.execute("SELECT * FROM device_status").fetchall()
    finally:
        conn.close()

    parent_uid = _ensure_user(PARENT_ALIAS, "Parent")
    student_uid = _ensure_user(STUDENT_ALIAS, "Student")
    _ensure_webapp_backend(parent_uid)
    _ensure_webapp_backend(student_uid)

    if allow_store.get_edge(parent_uid, student_uid) is None:
        allow_store.set_edge(parent_uid, student_uid, message=True, locate=True)
    if allow_store.get_edge(student_uid, parent_uid) is None:
        allow_store.set_edge(student_uid, parent_uid, message=True, locate=True)

    device_ids = {row["device_id"] for row in message_rows} | {
        row["device_id"] for row in status_rows
    }
    devices_created = 0
    for device_id in sorted(device_ids):
        if devices_store.get_device(device_id) is None:
            devices_store.create_device(
                device_id=device_id,
                owner_uid=student_uid,
                label=f"imported: {device_id}",
                mqtt_username=device_id,
                mqtt_password_hash="imported-unknown",
                default_to_uid=parent_uid,
            )
            devices_created += 1

    if devices_created:
        # Same ordering gap `app/routers/admin.py`'s `POST /api/admin/devices`
        # closes (`(build finding, Phase 4)`): the `locate` edges above were
        # written *before* these devices existed, and `set_edge` only ever
        # recomputes `locatableBy` on devices that exist at the moment the
        # edge changes -- without this, every imported device would stay at
        # `locatableBy: []` and the parent could not read its `locations`
        # through `firestore.rules`. See
        # `app.store.allow.recompute_locatable_by_for_owner`'s docstring.
        allow_store.recompute_locatable_by_for_owner(student_uid)

    messages_imported = 0
    for row in message_rows:
        created_at = datetime.fromtimestamp(row["created_at"], tz=UTC)
        if row["direction"] == "down":
            sender_uid, recipient_uid = parent_uid, student_uid
            state = _effective_state(row, now)
            deliveries = {
                "pager": {
                    "kind": "pager",
                    "state": state,
                    "attempts": 0,
                    "shownTs": row["shown_ts"],
                    "readTs": row["read_ts"],
                }
            }
        else:
            sender_uid, recipient_uid = student_uid, parent_uid
            deliveries = {}

        messages_store.create_message(
            sender_uid=sender_uid,
            recipient_uid=recipient_uid,
            kind="text",
            ts=row["ts"],
            body=row["body"],
            deliveries=deliveries,
            msg_id=row["id"],
            created_at=created_at,
        )
        messages_imported += 1

    return {
        "usersEnsured": 2,
        "devicesCreated": devices_created,
        "messagesImported": messages_imported,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("db_path", help="path to the MVP relay.db SQLite file")
    args = parser.parse_args(argv)

    result = import_sqlite(args.db_path)
    print(
        f"import complete: {result['usersEnsured']} users ensured, "
        f"{result['devicesCreated']} devices created, "
        f"{result['messagesImported']} messages imported"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

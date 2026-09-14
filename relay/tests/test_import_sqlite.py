"""`app.db.import_sqlite`: seeds a small MVP-shaped SQLite DB (the schema
`app/db/migrations/0001_init.sql` used to define -- reproduced inline here
since that file is retired, docs/SERVER_PLAN.md §3 "MVP import") and checks
the resulting Firestore users/backends/messages."""

from __future__ import annotations

import sqlite3
import time

import pytest

from app.db.import_sqlite import import_sqlite
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store

# Reproduced from the retired app/db/migrations/0001_init.sql -- this test
# is the one place that schema still needs to exist, to prove the importer
# reads it correctly.
SCHEMA = """
CREATE TABLE messages (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    id TEXT NOT NULL UNIQUE,
    device_id TEXT NOT NULL,
    direction TEXT NOT NULL CHECK (direction IN ('down', 'up')),
    v INTEGER NOT NULL DEFAULT 1,
    ts INTEGER NOT NULL,
    sender TEXT,
    body TEXT,
    state TEXT NOT NULL,
    shown_ts INTEGER,
    read_ts INTEGER,
    created_at INTEGER NOT NULL
);

CREATE TABLE device_status (
    device_id TEXT PRIMARY KEY,
    state TEXT NOT NULL,
    mode TEXT,
    batt_mv INTEGER,
    rssi INTEGER,
    session TEXT,
    ts INTEGER,
    fw TEXT,
    updated_at INTEGER NOT NULL
);
"""


@pytest.fixture
def seed_db(tmp_path) -> str:
    path = str(tmp_path / "relay.db")
    conn = sqlite3.connect(path)
    conn.executescript(SCHEMA)
    now = int(time.time())
    conn.execute(
        "INSERT INTO messages (id, device_id, direction, v, ts, sender, body, state, "
        "shown_ts, read_ts, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        ("m_11111111", "pgr-0001", "down", 1, now - 300, "parent", "pickup at 3", "read",
         now - 290, now - 280, now - 300),
    )
    conn.execute(
        "INSERT INTO messages (id, device_id, direction, v, ts, sender, body, state, "
        "shown_ts, read_ts, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        ("m_22222222", "pgr-0001", "down", 1, now - 200, "parent", "still stale", "queued",
         None, None, now - 200_000),  # old enough to be 'expired' at effective_state time
    )
    conn.execute(
        "INSERT INTO messages (id, device_id, direction, v, ts, sender, body, state, "
        "shown_ts, read_ts, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        ("u_33333333", "pgr-0001", "up", 1, now - 100, "student", "ok coming", "received",
         None, None, now - 100),
    )
    conn.execute(
        "INSERT INTO device_status (device_id, state, mode, batt_mv, rssi, session, ts, fw, "
        "updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
        ("pgr-0001", "online", "sleep", 3300, -90, "s_1", now, "0.1.0", now),
    )
    conn.commit()
    conn.close()
    return path


def test_import_creates_parent_and_student_users(seed_db: str):
    result = import_sqlite(seed_db)
    assert result["messagesImported"] == 3
    assert result["devicesCreated"] == 1

    parent = users_store.get_user_by_alias("parent")
    student = users_store.get_user_by_alias("student")
    assert parent is not None
    assert student is not None
    assert parent.displayName == "Parent"
    assert student.displayName == "Student"


def test_import_gives_each_user_a_webapp_backend(seed_db: str):
    import_sqlite(seed_db)
    parent = users_store.get_user_by_alias("parent")
    student = users_store.get_user_by_alias("student")
    assert any(b.kind == "webapp" for b in backends_store.list_backends(parent.uid))
    assert any(b.kind == "webapp" for b in backends_store.list_backends(student.uid))


def test_import_creates_device_owned_by_student(seed_db: str):
    import_sqlite(seed_db)
    student = users_store.get_user_by_alias("student")
    parent = users_store.get_user_by_alias("parent")
    device = devices_store.get_device("pgr-0001")
    assert device is not None
    assert device.ownerUid == student.uid
    assert device.defaultToUid == parent.uid


def test_import_creates_allow_edges_both_directions(seed_db: str):
    import_sqlite(seed_db)
    parent = users_store.get_user_by_alias("parent")
    student = users_store.get_user_by_alias("student")
    edge1 = allow_store.get_edge(parent.uid, student.uid)
    edge2 = allow_store.get_edge(student.uid, parent.uid)
    assert edge1 is not None and edge1.message and edge1.locate
    assert edge2 is not None and edge2.message and edge2.locate


def test_import_folds_ack_history_into_deliveries(seed_db: str):
    import_sqlite(seed_db)
    parent = users_store.get_user_by_alias("parent")
    student = users_store.get_user_by_alias("student")

    msg = messages_store.get_message("m_11111111")
    assert msg is not None
    assert msg.senderUid == parent.uid
    assert msg.recipientUid == student.uid
    assert msg.deliveries["pager"].state == "read"

    stale = messages_store.get_message("m_22222222")
    assert stale is not None
    # still 'queued' in the old DB, but old enough to have expired -- the
    # importer computes the *effective* state, like the MVP did lazily.
    assert stale.deliveries["pager"].state == "expired"

    up_msg = messages_store.get_message("u_33333333")
    assert up_msg is not None
    assert up_msg.senderUid == student.uid
    assert up_msg.recipientUid == parent.uid
    assert up_msg.body == "ok coming"


def test_import_thread_preserves_original_insertion_order(seed_db: str):
    import_sqlite(seed_db)
    parent = users_store.get_user_by_alias("parent")
    student = users_store.get_user_by_alias("student")
    thread = messages_store.list_thread(messages_store.conv_key(parent.uid, student.uid))
    assert [m.id for m in thread] == ["m_11111111", "m_22222222", "u_33333333"]


def test_import_is_safe_to_run_twice(seed_db: str):
    import_sqlite(seed_db)
    result2 = import_sqlite(seed_db)
    # Users/backends/allow/device are all idempotent no-ops the second time.
    assert result2["usersEnsured"] == 2
    assert result2["devicesCreated"] == 0

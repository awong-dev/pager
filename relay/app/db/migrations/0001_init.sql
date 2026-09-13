-- Initial schema for the relay message store.
-- `messages` holds both directions (direction='down'|'up'); thread order for
-- the parent UI is `seq` (insertion order), per PROTOCOL.md §3.5 -- NOT `ts`.

CREATE TABLE IF NOT EXISTS messages (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    id TEXT NOT NULL UNIQUE,
    device_id TEXT NOT NULL,
    direction TEXT NOT NULL CHECK (direction IN ('down', 'up')),
    v INTEGER NOT NULL DEFAULT 1,
    ts INTEGER NOT NULL,
    sender TEXT,
    body TEXT,
    -- down: queued -> sent -> shown -> read ('expired' is computed lazily,
    --       never persisted -- see app/store.py MessageRow.effective_state).
    -- up:   always 'received' (no relay-side lifecycle, PROTOCOL.md §4.2).
    state TEXT NOT NULL,
    shown_ts INTEGER,
    read_ts INTEGER,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_messages_device_seq ON messages (device_id, seq);
CREATE INDEX IF NOT EXISTS idx_messages_republish ON messages (device_id, direction, state, created_at);

CREATE TABLE IF NOT EXISTS device_status (
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

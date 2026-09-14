"""Firestore-backed store, split per docs/SERVER_PLAN.md §3's collections:
one module per top-level collection (`users`, `devices`, `backends`,
`allow`, `messages`, `locations`, `settings`). Replaces the MVP's
`app/store.py` (SQLite) -- see `app/db/import_sqlite.py` for the one-off
migration of old data into these shapes.

Each submodule owns its own Firestore access; nothing here re-exports them
under new names on purpose (`from app.store import users` is the intended
import shape, so it is obvious in a call site which collection a piece of
code is touching).
"""

from __future__ import annotations

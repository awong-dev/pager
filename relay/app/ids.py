"""Message id generation, per docs/PROTOCOL.md §1."""

from __future__ import annotations

import os


def new_id(prefix: str) -> str:
    """`prefix` + 8 lowercase hex chars from a CSPRNG (32 bits of os.urandom)."""
    return f"{prefix}{os.urandom(4).hex()}"


def new_message_id() -> str:
    """Relay-originated (down) message id: `m_` + 8 lowercase hex."""
    return new_id("m_")

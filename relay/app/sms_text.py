"""Text helpers shared by every SMS/Voice path (bridge inbound, held-text
approval, Chat subscribe) -- docs/PROTOCOL.md §3.1 body rules and the Google
Voice thread link."""

from __future__ import annotations

import re

from app.wire import BODY_MAX_CODEPOINTS

SMS_BODY_MAX_CODEPOINTS = BODY_MAX_CODEPOINTS  # 160
SMS_BODY_MAX_UTF8_BYTES = 320  # docs/PROTOCOL.md §3.1

_CONTROL_RE = re.compile("[\u0000-\u001f\u007f]")


def voice_link(e164: str) -> str:
    """O1 (revised): the Voice web app addresses a thread by the peer's
    number, so tier 2 can start a text to any number."""
    return f"https://voice.google.com/u/0/messages?itemId=t.{e164}"


def pager_body(raw: str) -> str:
    """docs/PROTOCOL.md §3.1: each control character (U+0000-U+001F, U+007F)
    becomes a space, nothing else is collapsed, then strip."""
    return _CONTROL_RE.sub(" ", raw).strip()


def body_too_long(body: str) -> bool:
    """More than 160 code points or 320 UTF-8 bytes (§3.1)."""
    return (
        len(body) > SMS_BODY_MAX_CODEPOINTS or len(body.encode("utf-8")) > SMS_BODY_MAX_UTF8_BYTES
    )


def too_long_hint(_n: int | None = None) -> str:
    return f"Message too long (max {SMS_BODY_MAX_CODEPOINTS} characters) -- not sent."


def redact_phone(phone: str) -> str:
    """Last-4-digits only: the full number is PII nothing downstream needs."""
    return f"...{phone[-4:]}" if len(phone) >= 4 else "..."

"""Carrier-facing SMS text and keyword handling -- docs/RELAY_SMS_DESIGN.md
decision 11. Pure functions and constants; the relay owns opt-in/opt-out for
the Twilio number (Twilio's Advanced Opt-Out must be off, or STOP/START/HELP
never reach the webhook).

The operator name and support email come from `SMS_OPERATOR_NAME` /
`SMS_SUPPORT_EMAIL` (read at call time); the program name is fixed.
"""

from __future__ import annotations

import os
import re
from typing import Literal

PROGRAM = "Pager"
DEFAULT_OPERATOR = "Albert Wong"
DEFAULT_SUPPORT_EMAIL = "awong.dev@gmail.com"

# Appended verbatim after the suffix on the first relayed message to a number
# per UTC calendar day.
DISCLOSURE = ". Reply STOP to opt out, HELP for help."

Keyword = Literal["start", "stop", "help"]

_KEYWORDS: dict[str, Keyword] = {
    "START": "start",
    "OPTIN": "start",
    "IN": "start",
    "STOP": "stop",
    "UNSUBSCRIBE": "stop",
    "END": "stop",
    "QUIT": "stop",
    "HELP": "help",
    "INFO": "help",
    "SUPPORT": "help",
}


def operator_name() -> str:
    return os.environ.get("SMS_OPERATOR_NAME") or DEFAULT_OPERATOR


def support_email() -> str:
    return os.environ.get("SMS_SUPPORT_EMAIL") or DEFAULT_SUPPORT_EMAIL


def welcome() -> str:
    return (
        f"Welcome to {PROGRAM}, run by {operator_name()}. You'll receive two-way coordination "
        "messages relayed from a Pager device. Message frequency varies. Msg & data rates may "
        "apply. Reply HELP for help, STOP to opt out."
    )


def opt_out_reply() -> str:
    return (
        f"You have been unsubscribed from {PROGRAM} ({operator_name()}). You will receive no "
        f"further messages. Email {support_email()} with any questions."
    )


def help_reply() -> str:
    return (
        f"{PROGRAM} ({operator_name()}): two-way coordination messages relayed from a Pager "
        f"device. For help, email {support_email()}. Msg & data rates may apply. Reply STOP to "
        "opt out."
    )


def __getattr__(name: str) -> str:
    """`WELCOME`, `OPT_OUT_REPLY`, `HELP_REPLY` as constants that still follow
    the env values (resolved on access, not at import)."""
    if name == "WELCOME":
        return welcome()
    if name == "OPT_OUT_REPLY":
        return opt_out_reply()
    if name == "HELP_REPLY":
        return help_reply()
    raise AttributeError(name)


def relay_body(sender_display_name: str, text: str) -> str:
    return f'{sender_display_name} says: "{text}" - {PROGRAM} ({operator_name()})'


def keyword(body: str) -> Keyword | None:
    return _KEYWORDS.get(body.strip().upper())


# ---------------------------------------------------------------------------
# defang -- URLs and phone numbers in relayed text get human-undoable spaces
# ---------------------------------------------------------------------------

_LABEL = r"[A-Za-z0-9-]+"
# scheme://host (host ends at / ? # : or whitespace), www.host, or bare host.tld.
_URL_RE = re.compile(
    r"(?P<scheme>[A-Za-z][A-Za-z0-9+.\-]*://)(?P<shost>[^/?#:\s]+)"
    rf"|(?P<www>(?<![A-Za-z0-9-])www\.{_LABEL}(?:\.{_LABEL})*)"
    rf"|(?P<bare>(?<![A-Za-z0-9-]){_LABEL}(?:\.{_LABEL})*\.[A-Za-z]{{2,}}(?![A-Za-z0-9-]))"
)
_PHONE_RE = re.compile(r"[\d(][\d\-.() ]*")


def _space_dots(host: str) -> str:
    return re.sub(r"\.(?=.)", ". ", host)


def _defang_url(m: re.Match[str]) -> str:
    if m.group("scheme") is not None:
        return m.group("scheme") + " " + _space_dots(m.group("shost"))
    return _space_dots(m.group(0))


def _defang_phone(m: re.Match[str]) -> str:
    span = m.group(0)
    core = span.rstrip(" .-(")
    tail = span[len(core) :]
    if sum(c.isdigit() for c in core) < 7:
        return span
    out: list[str] = []
    for chunk in core.split(" "):
        if len(chunk) > 3:
            chunk = " ".join(chunk[i : i + 3] for i in range(0, len(chunk), 3))
        out.append(chunk)
    return " ".join(out) + tail


def defang(text: str) -> str:
    """URLs first, then phone numbers (see module docs); idempotent."""
    return _PHONE_RE.sub(_defang_phone, _URL_RE.sub(_defang_url, text))

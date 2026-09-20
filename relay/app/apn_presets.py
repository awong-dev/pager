"""Carrier APN presets offered when a device is created or edited.

Why this exists (found on hardware, 2026-09-20): a pager that attaches with a
blank APN lets the network pick one, and on at least one carrier the default
is a crippled data path -- on a US Mobile "Dark Star" SIM (an AT&T reseller)
small plain TCP works but every TLS handshake stalls, until the APN is set to
`ereseller`. The SIM cannot be identified reliably from the device: its IMSI
and ICCID ranges are AT&T's own, it has no operator-name file, and its GID1
(0x20) is shared by other AT&T resellers. So the person setting the pager up
chooses. `docs/BRINGUP_NOTES.md` has the measurements.

The APN travels in the typed setup code (`... @ host;apn=<apn>`, used for the
bootstrap attach) and in the encrypted bundle (`apn`, used for ever after), so
a change takes effect the next time the pager is set up.

Add a preset only with a source for its APN string and, ideally, a pager that
has been seen working on it.
"""

from __future__ import annotations

import re

from pydantic import BaseModel

# ident.h: IDENT_APN_MAX is 32 including the NUL. 3GPP TS 23.003 §9.1: labels
# of letters, digits and hyphens separated by dots.
APN_MAX_LEN = 31
_APN_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$")


class ApnPreset(BaseModel):
    id: str
    label: str
    apn: str
    note: str | None = None


PRESETS: list[ApnPreset] = [
    ApnPreset(
        id="us-mobile-dark-star",
        label="US Mobile - Dark Star (AT&T)",
        apn="ereseller",
        note="Required: with the carrier default, TLS never connects on this SIM.",
    ),
]


def validate_apn(apn: str | None) -> str | None:
    """Returns the normalised APN, or None for "carrier default". Raises
    ValueError with a human-readable message otherwise."""
    if apn is None:
        return None
    apn = apn.strip()
    if apn == "":
        return None
    if len(apn) > APN_MAX_LEN:
        raise ValueError(f"APN is longer than {APN_MAX_LEN} characters")
    if not _APN_RE.match(apn) or ".." in apn:
        raise ValueError("APN may contain only letters, digits, dots and hyphens")
    return apn

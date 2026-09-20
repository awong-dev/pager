"""`GET /ca/{sha256hex}.pem` -- docs/V02_DESIGN.md §4.4 / docs/CA_TRUST_PLAN.md
§3.4.

Public, unauthenticated, on purpose: the CA is trusted by its hash, not by
this endpoint. The hash arrives at the device over a channel that is already
signed (a `/down cfg.ca` push, HMAC-verified per §14) or already encrypted
(the bootstrap bundle, §3.2), so "the download needs no server
authentication at all" (`CA_TRUST_PLAN.md` §3.4) -- adding auth here would
buy nothing but a second thing that can misconfigure and take pages down.

404 unless the hash names a CA this relay has ever served (`app/store/
cas.py`, populated by `app/ca_resolve.py`'s `ca_pointer()`), never 500 on a
malformed path segment -- a scanner probing `/ca/../etc/passwd.pem` or any
non-hex string is just another 404, not a stack trace.

Immutable cache headers: the URL is content-addressed (the hash *is* the
content), so a successful response can be cached forever -- this also means
the one real cost of serving this route (a Firestore read) is paid at most
once per CA per intermediate cache, not once per device fetch.
"""

from __future__ import annotations

import re

from fastapi import APIRouter, HTTPException, Response

from app.store import cas as cas_store

router = APIRouter()

# The relay always names the pointer by a plain SHA-256 hex digest (64 lower-
# case hex chars, app/ca_resolve.py's `hashlib.sha256(...).hexdigest()`);
# anything else cannot be a hash this relay ever issued.
_SHA256_HEX_RE = re.compile(r"^[0-9a-f]{64}$")


@router.get("/ca/{sha256hex}.pem")
def get_ca_pem(sha256hex: str) -> Response:
    if not _SHA256_HEX_RE.match(sha256hex):
        raise HTTPException(status_code=404, detail="not found")
    pem = cas_store.get_pem(sha256hex)
    if pem is None:
        raise HTTPException(status_code=404, detail="not found")
    return Response(
        content=pem,
        media_type="text/plain",
        headers={"Cache-Control": "public, max-age=31536000, immutable"},
    )

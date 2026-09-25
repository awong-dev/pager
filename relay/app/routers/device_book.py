"""`GET /api/device/book` -- docs/PROTOCOL.md §3.7 (book pull, v0.4) /
§14.7 (authenticating the request, response signature).

No `require_user`/`require_admin` dependency: this route is device-facing,
authenticated by the device's own HMAC key (§14.7), the same shape
`app/routers/ca.py`'s `GET /ca/{sha}.pem` and `POST /webhooks/mqtt` already
use for "the caller is not a Firebase-Auth'd human". Unlike `ca.py`'s public
CA pointer, this endpoint *does* need real authentication -- the book lists
who is allowed to message this device's owner, which is not public.

`request_tag`/`sign_cbor` do the actual cryptography (`app/devauth.py`);
`devcfg.build_book_body` builds the unsigned CBOR-map body (`app/devcfg.py`);
`record_bad_sig` (factored out of `app.ingest.Ingest._verify_and_decode`,
the same §14.4 bad-signature accounting) is reused verbatim for step 3's
`401` so a bad request tag counts against the same `sigFailures`/`authAlarm`
a bad MQTT envelope signature would.

Order is exactly §14.7's "Relay, in order" list -- see each numbered
comment below."""

from __future__ import annotations

import base64
import hmac
import logging
import re

from fastapi import APIRouter, HTTPException, Request, Response

from app import devauth, devcfg
from app.ingest import record_bad_sig
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.wire import DEVICE_ID_RE, N_MAX_EXCLUSIVE

logger = logging.getLogger("relay.device_book")

router = APIRouter(prefix="/api/device")

# §14.7: "`X-N` ... decimal, no leading zeros." No such rule is stated for
# `bv` (§3.7's own range, 0..2**32-1) -- only its range is normative, so
# `_BV_RE` allows a leading zero and `_N_RE` does not.
_N_RE = re.compile(r"^(0|[1-9]\d*)$")
_BV_RE = re.compile(r"^\d+$")
# §14.3/§14.7: 8 raw tag bytes, base64url, no padding, is always exactly 11
# characters -- `app/devauth.py`'s own `_JSON_SIG_RE` comment spells out the
# same arithmetic for the envelope `sig` field.
_SIG_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")

_BV_MAX_EXCLUSIVE = 2**32

_BOOK_TOPIC = "/api/device/book"


@router.get("/book")
def get_book(request: Request, bv: str | None = None) -> Response:
    device_id = request.headers.get("X-Device-Id", "")
    n_raw = request.headers.get("X-N", "")
    sig_raw = request.headers.get("X-Sig", "")

    # 1. "A missing or ill-formed header, or `bv` not a decimal in
    #    0...2**32-1 -> 400."
    if (
        not DEVICE_ID_RE.match(device_id)
        or bv is None
        or not _BV_RE.match(bv)
        or not (0 <= int(bv) < _BV_MAX_EXCLUSIVE)
        or not _N_RE.match(n_raw)
        or not (0 <= int(n_raw) < N_MAX_EXCLUSIVE)
        or not _SIG_RE.match(sig_raw)
    ):
        raise HTTPException(status_code=400, detail="malformed request")

    bv_int = int(bv)
    n = int(n_raw)

    # 2. "`devices/{X-Device-Id}` missing or revoked, or `authMode` !=
    #    `hmac` -> 404." (a `hmac` device with no `deviceSecrets/{d}` row is
    #    the same fail-closed 404 `app/broker.py`'s `publish_down` gives on
    #    the down side -- nothing to verify against.)
    device = devices_store.get_device(device_id)
    if device is None or device.revokedAt is not None or device.authMode != "hmac":
        raise HTTPException(status_code=404, detail="not found")
    secret = device_secrets_store.get(device_id)
    if secret is None:
        raise HTTPException(status_code=404, detail="not found")

    # 3. "Tag verified in constant time; failure -> 401, counted in
    #    `sigFailures` and the §14.4 alarm window exactly like a bad
    #    envelope signature."
    expected = devauth.request_tag(secret.hmacKey, device_id, n, bv_int)
    got = base64.urlsafe_b64decode(sig_raw + "=" * (-len(sig_raw) % 4))
    if not hmac.compare_digest(got, expected):
        logger.warning(
            "SECURITY bad-sig device=%s endpoint=%s n=%s bv=%s", device_id, _BOOK_TOPIC, n, bv_int
        )
        record_bad_sig(device_id)
        raise HTTPException(status_code=401, detail="bad signature")

    # 4. "One Firestore transaction on `deviceSecrets/{d}`: require
    #    `n > upN`, then set `upN = n` and shift `upBits` ... otherwise
    #    `409`, nothing written."
    if not device_secrets_store.accept_request_n(device_id, n):
        raise HTTPException(status_code=409, detail="replay")

    # 5. "200 with §3.7's body. A store failure after step 4 -> 503; that
    #    `n` is spent." -- `n` is consumed by step 4 regardless of what
    #    happens next, so any failure building/signing the body below must
    #    not look like a client error.
    try:
        body = devcfg.build_book_body(device_id)
    except Exception:
        logger.exception("device book store error device=%s n=%s (n already consumed)", device_id, n)
        raise HTTPException(status_code=503, detail="temporarily unavailable") from None

    # "`n` echoes the request's `X-N`" -- placed last so it is the last pair
    # before `sig` (§14.3's "`sig` must be the last pair", unaffected by
    # this), matching every other signed envelope's field-order convention.
    body["n"] = n
    payload = devauth.sign_cbor(secret.hmacKey, _BOOK_TOPIC, body)

    logger.info(
        "device book device=%s requested_bv=%s served_bv=%s contacts=%s bytes=%s",
        device_id,
        bv_int,
        body["bv"],
        len(body["c"]),
        len(payload),
    )

    return Response(
        content=payload,
        media_type="application/cbor",
        headers={"Cache-Control": "private, no-store"},
    )

"""`cas/{sha256hex}` -- docs/V02_DESIGN.md §4.4 / docs/CA_TRUST_PLAN.md §3.4.

Every CA PEM the relay has ever handed out a pointer for (in a bootstrap
bundle's `ca_url`, or a `/down cfg.ca` push), keyed by the hex SHA-256 digest
of the PEM text itself -- content-addressed, so `remember()` is naturally
idempotent (re-storing the same PEM under the same key is a no-op write of
identical content) and there is nothing else to key on. This is what makes
`GET /ca/{sha256hex}.pem` (`app/routers/ca.py`) keep resolving an old
pointer even after the relay's own *current* CA (`BROKER_CA_PEM`/
`app/ca_resolve.py`'s auto-resolve) changes -- the whole point of a pointer
that travelled inside a signed/encrypted message once and must still be
honoured however long that device keeps it.

Server-only in the sense that matters for `firestore.rules`: nothing in this
collection is *secret* (a CA is public by construction), so it deliberately
has no `match` block giving a client direct Firestore read access either --
the one and only public read path is the HTTP route above, which can apply
its own cache headers and 404-vs-200 semantics; a raw Firestore read would
bypass both. `relay/tests/test_rules.py` pins the resulting default-deny
(same posture as `deviceSecrets`/`phoneIndex`).
"""

from __future__ import annotations

from google.cloud.firestore import SERVER_TIMESTAMP

from app.db.firestore import get_db


def _cas():
    return get_db().collection("cas")


def remember(sha256hex: str, pem: str) -> None:
    """Idempotent: writes `{pem, createdAt}`, merging so a re-remember of an
    already-known hash does not disturb its original `createdAt`... except
    that `SERVER_TIMESTAMP` inside a `merge=True` `.set()` always overwrites
    the field it names, by Firestore's own semantics -- which is fine here,
    since nothing reads `createdAt` for anything but human debugging and the
    `pem` for a given hash can never legitimately change (the hash *is* the
    content)."""
    _cas().document(sha256hex).set({"pem": pem, "createdAt": SERVER_TIMESTAMP}, merge=True)


def get_pem(sha256hex: str) -> str | None:
    snap = _cas().document(sha256hex).get()
    if not snap.exists:
        return None
    pem = (snap.to_dict() or {}).get("pem")
    return pem if isinstance(pem, str) else None

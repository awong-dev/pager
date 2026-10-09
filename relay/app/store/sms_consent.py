"""`smsConsent/{e164}` -- per-number SMS opt-in state, owned by the relay
(docs/RELAY_SMS_DESIGN.md decision 11).

Server-only (no `firestore.rules` match; default-deny). Fields: `status`
(`opted_in` | `opted_out`), `optedInAt` / `optedOutAt` (server timestamps,
nullable), `source` (`keyword` | `admin`), `lastDisclosureDate` (UTC
`YYYY-MM-DD` of the last relayed message that carried the STOP/HELP
disclosure) and `updatedAt`.
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from google.cloud.firestore import SERVER_TIMESTAMP, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction

ConsentStatus = Literal["opted_in", "opted_out"]
ConsentSource = Literal["keyword", "admin"]


class Consent(BaseModel):
    model_config = ConfigDict(extra="ignore")

    status: ConsentStatus
    optedInAt: datetime | None = None
    optedOutAt: datetime | None = None
    source: ConsentSource | None = None
    lastDisclosureDate: str | None = None
    updatedAt: datetime | None = None


def _ref(e164: str):
    return get_db().collection("smsConsent").document(e164)


def get(e164: str) -> Consent | None:
    snap = _ref(e164).get()
    if not snap.exists:
        return None
    return Consent.model_validate(snap.to_dict() or {})


def is_opted_in(e164: str) -> bool:
    row = get(e164)
    return row is not None and row.status == "opted_in"


def mark_opted_in(e164: str, *, source: ConsentSource) -> bool:
    """Upsert; True iff the status changed to `opted_in` (was missing or
    `opted_out`) -- the caller sends the welcome only then."""
    ref = _ref(e164)

    def _txn(transaction: Transaction) -> bool:
        snap = ref.get(transaction=transaction)
        current = (snap.to_dict() or {}).get("status") if snap.exists else None
        if current == "opted_in":
            return False
        transaction.set(
            ref,
            {
                "status": "opted_in",
                "optedInAt": SERVER_TIMESTAMP,
                "source": source,
                "updatedAt": SERVER_TIMESTAMP,
            },
            merge=True,
        )
        return True

    return run_transaction(_txn)


def mark_opted_out(e164: str) -> None:
    _ref(e164).set(
        {"status": "opted_out", "optedOutAt": SERVER_TIMESTAMP, "updatedAt": SERVER_TIMESTAMP},
        merge=True,
    )


def claim_disclosure(e164: str, today: str) -> bool:
    """True iff `lastDisclosureDate != today`, and records `today`."""
    ref = _ref(e164)

    def _txn(transaction: Transaction) -> bool:
        snap = ref.get(transaction=transaction)
        last = (snap.to_dict() or {}).get("lastDisclosureDate") if snap.exists else None
        if last == today:
            return False
        transaction.set(ref, {"lastDisclosureDate": today, "updatedAt": SERVER_TIMESTAMP}, merge=True)
        return True

    return run_transaction(_txn)

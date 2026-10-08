#!/usr/bin/env python3
"""Purge the Firestore leftovers of a deleted device from production.

Why this exists: the relay's `DELETE /api/admin/devices/{id}` (and the web
UI's Revoke/Delete) removes only `devices/{id}`, the owner's pager backend and
the EMQX user. Everything else keyed by the device id stays behind. This
script removes it: `deviceSecrets/{id}`, `devices/{id}/{locations,battery,
smsLog}`, `locReqs/{id}`, `locWireIds` and `contactRequests` and `setupCodes`
docs whose `deviceId` field is the id, the id inside `messages.pendingDeviceIds`
(ArrayRemove, the messages themselves stay), every `users/{uid}/backends/*`
with kind "pager" and config.deviceId == id, and finally `devices/{id}`.

Auth: Application Default Credentials (`gcloud auth application-default login`
as an owner of the project), same init as tools/set_web_password.py. Dry run
by default output only; nothing is written without --yes. Refuses `proto2`.

    relay/.venv/bin/python tools/prod_device_purge.py proto3 --dry-run
    relay/.venv/bin/python tools/prod_device_purge.py proto3 --yes
"""

from __future__ import annotations

import argparse
import sys

import firebase_admin
from firebase_admin import credentials, firestore
from google.cloud.firestore_v1 import ArrayRemove
from google.cloud.firestore_v1.base_query import FieldFilter

BATCH = 400
PROTECTED = {"proto2"}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("device_id")
    ap.add_argument("--project", default="kid-pager")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--yes", action="store_true")
    args = ap.parse_args(argv[1:])
    did: str = args.device_id
    if did in PROTECTED or not did or "/" in did:
        print(f"refusing to purge {did!r}", file=sys.stderr)
        return 2
    if args.dry_run == args.yes:
        print("pass exactly one of --dry-run or --yes", file=sys.stderr)
        return 2

    firebase_admin.initialize_app(credentials.ApplicationDefault(), {"projectId": args.project})
    db = firestore.client()

    def eq(coll: str, field: str):
        return db.collection(coll).where(filter=FieldFilter(field, "==", did)).stream()

    dev = db.collection("devices").document(did)
    deletes: dict[str, list] = {
        "deviceSecrets": [r for r in [db.collection("deviceSecrets").document(did)] if r.get().exists],
        f"devices/{did}/locations": [s.reference for s in dev.collection("locations").stream()],
        f"devices/{did}/battery": [s.reference for s in dev.collection("battery").stream()],
        f"devices/{did}/smsLog": [s.reference for s in dev.collection("smsLog").stream()],
        "locReqs": [r for r in [db.collection("locReqs").document(did)] if r.get().exists],
        "locWireIds": [s.reference for s in eq("locWireIds", "deviceId")],
        "contactRequests": [s.reference for s in eq("contactRequests", "deviceId")],
        "setupCodes": [s.reference for s in eq("setupCodes", "deviceId")],
    }
    backends = []
    for u in db.collection("users").stream():
        for b in u.reference.collection("backends").stream():
            d = b.to_dict() or {}
            if d.get("kind") == "pager" and (d.get("config") or {}).get("deviceId") == did:
                backends.append(b.reference)
    deletes["users/*/backends"] = backends
    pending = [
        s.reference
        for s in db.collection("messages")
        .where(filter=FieldFilter("pendingDeviceIds", "array_contains", did))
        .stream()
    ]
    device_exists = dev.get().exists
    deletes["devices"] = [dev] if device_exists else []

    for name, refs in deletes.items():
        print(f"{name}: delete {len(refs)}")
        for r in refs:
            print(f"  {r.path}")
    print(f"messages.pendingDeviceIds: modify {len(pending)}")
    for r in pending:
        print(f"  {r.path}")

    if args.dry_run:
        return 0

    def run(ops: list) -> None:
        for i in range(0, len(ops), BATCH):
            batch = db.batch()
            for kind, ref in ops[i : i + BATCH]:
                if kind == "del":
                    batch.delete(ref)
                else:
                    batch.update(ref, {"pendingDeviceIds": ArrayRemove([did])})
            batch.commit()

    ops = [("upd", r) for r in pending]
    for name, refs in deletes.items():
        if name != "devices":
            ops += [("del", r) for r in refs]
    run(ops)
    run([("del", r) for r in deletes["devices"]])  # last
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

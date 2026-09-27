"use client";

/**
 * Live list of devices this account is authorized to see the location of --
 * `devices` filtered on `locatableBy array-contains me.uid`, exactly the
 * field `firestore.rules`' `devices/{d}/locations/{l}` rule checks
 * (denormalised by `relay/app/store/allow.py`'s `_recompute_locatable_by`
 * from the admin-managed `allow/{fromUid}_{toUid}.locate` edges,
 * `web/app/admin/allowlist/page.tsx`). A non-admin cannot grant themselves
 * this -- `allow` is written only by `PUT /api/admin/allowlist`
 * (`app/routers/admin.py`'s `replace_all` route), which is gated on the
 * `admin` custom claim same as every other `/api/admin/*` route, and is
 * otherwise unwritable by any client (`firestore.rules`' closing
 * `allow write: if false`).
 *
 * Deliberately excludes a device's own owner: `firestore.rules`'
 * `locations` subcollection rule checks *only* `locatableBy`, with no
 * `ownerUid`/`isAdmin()` bypass (unlike the top-level `devices/{d}` read
 * rule, which has both) -- so today's model genuinely does not let an
 * owner or an admin read a device's location history "for free"; they too
 * need an explicit `locate` edge, same as anyone else. This hook mirrors
 * that boundary rather than working around it.
 *
 * Shared by `AppShell` (nav item visibility -- "Location" only shows once
 * this list is non-empty) and `/location` (the actual picker), so there is
 * exactly one query for "can I locate anyone" instead of two that could
 * drift.
 */

import { useEffect, useState } from "react";
import { collection, onSnapshot, query, where } from "firebase/firestore";

import { useAuth } from "./auth-context";
import { getFirestoreDb } from "./firebase";
import type { DeviceDoc } from "./types";

export interface LocatableDevice extends DeviceDoc {
  id: string;
}

export function useLocatableDevices(): { devices: LocatableDevice[]; loaded: boolean } {
  const { me } = useAuth();
  const [devices, setDevices] = useState<LocatableDevice[]>([]);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    if (!me) {
      // Signed out: leave whatever the last snapshot held rather than
      // setState-ing synchronously in the effect body (same
      // `react-hooks/set-state-in-effect` shape `lib/directory.tsx`'s group
      // listener uses) -- harmless, since `RequireAuth` unmounts every
      // reader of this hook before `me` ever goes null.
      return;
    }
    const q = query(
      collection(getFirestoreDb(), "devices"),
      where("locatableBy", "array-contains", me.uid)
    );
    const unsubscribe = onSnapshot(q, (snap) => {
      setDevices(snap.docs.map((d) => ({ id: d.id, ...(d.data() as DeviceDoc) })));
      setLoaded(true);
    });
    return unsubscribe;
  }, [me]);

  return { devices, loaded };
}

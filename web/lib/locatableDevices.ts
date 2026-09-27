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
 * Plus the viewer's *own* devices (`ownerUid == me.uid`, flagged `isOwn`):
 * owner decision, 27 Sep 2026 -- `firestore.rules`' `locations` rule lets
 * a device's owner read it, and `POST /conversations/{alias}/locate` lets
 * the owner locate it, with no `locate` edge. Still no admin bypass: an
 * admin sees only devices they own or hold a `locate` edge for. The two
 * listeners are merged by device id (own wins), so a device that is both
 * owned and in `locatableBy` appears once.
 *
 * Shared by `AppShell` (nav item visibility -- "Location" only shows once
 * this list is non-empty) and `/location` (the actual picker), so there is
 * exactly one query for "can I locate anyone" instead of two that could
 * drift.
 */

import { useEffect, useMemo, useState } from "react";
import { collection, onSnapshot, query, where } from "firebase/firestore";

import { useAuth } from "./auth-context";
import { getFirestoreDb } from "./firebase";
import type { DeviceDoc } from "./types";

export interface LocatableDevice extends DeviceDoc {
  id: string;
  /** True when the viewer owns this device (listed as "You"). */
  isOwn: boolean;
}

export function useLocatableDevices(): { devices: LocatableDevice[]; loaded: boolean } {
  const { me } = useAuth();
  const [granted, setGranted] = useState<LocatableDevice[]>([]);
  const [own, setOwn] = useState<LocatableDevice[]>([]);
  const [grantedLoaded, setGrantedLoaded] = useState(false);
  const [ownLoaded, setOwnLoaded] = useState(false);

  useEffect(() => {
    if (!me) {
      // Signed out: leave whatever the last snapshot held rather than
      // setState-ing synchronously in the effect body (same
      // `react-hooks/set-state-in-effect` shape `lib/directory.tsx`'s group
      // listener uses) -- harmless, since `RequireAuth` unmounts every
      // reader of this hook before `me` ever goes null.
      return;
    }
    const devicesRef = collection(getFirestoreDb(), "devices");
    const unsubGranted = onSnapshot(
      query(devicesRef, where("locatableBy", "array-contains", me.uid)),
      (snap) => {
        setGranted(snap.docs.map((d) => ({ id: d.id, ...(d.data() as DeviceDoc), isOwn: false })));
        setGrantedLoaded(true);
      }
    );
    const unsubOwn = onSnapshot(query(devicesRef, where("ownerUid", "==", me.uid)), (snap) => {
      setOwn(snap.docs.map((d) => ({ id: d.id, ...(d.data() as DeviceDoc), isOwn: true })));
      setOwnLoaded(true);
    });
    return () => {
      unsubGranted();
      unsubOwn();
    };
  }, [me]);

  const devices = useMemo(() => {
    const ownIds = new Set(own.map((d) => d.id));
    return [...own, ...granted.filter((d) => !ownIds.has(d.id))];
  }, [own, granted]);

  return { devices, loaded: grantedLoaded && ownLoaded };
}

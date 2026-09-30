"use client";

/**
 * Live list of devices this account is authorized to see the location of --
 * docs/FAMILIES_DESIGN.md §1 decision 5, §5.3: location never crosses a
 * family, so every `devices` query here carries a `familyId ==
 * useFamily().familyId` filter -- both because the rule (`firestore.rules`'
 * `devices/{d}` match) requires `sameFam(familyId)` even for the owner
 * clause, which Firestore can only prove for a `list` query when the filter
 * itself is present, and because it is what actually keeps another family's
 * devices out of this list for a super who has switched scope.
 *
 * Three sources, merged by device id (own wins over an edge, which wins
 * over the family-wide listing):
 *  - own devices (`ownerUid == me.uid`, flagged `isOwn`) -- owner decision,
 *    27 Sep 2026 -- `firestore.rules`' `locations` rule lets a device's
 *    owner read it, and `POST /conversations/{alias}/locate` lets the
 *    owner locate it, with no `locate` edge.
 *  - granted devices (`locatableBy array-contains me.uid`), denormalised by
 *    `relay/app/store/allow.py`'s `_recompute_locatable_by` from the
 *    admin-managed `allow/{fromUid}_{toUid}.locate` edges
 *    (`web/app/admin/allowlist/page.tsx`). A non-admin cannot grant
 *    themselves this -- `allow` is written only by `PUT
 *    /api/admin/allowlist`, gated on the `admin`/`super` claim, and is
 *    otherwise unwritable by any client (`firestore.rules`' closing `allow
 *    write: if false`).
 *  - for a family admin (including super, who reads a chosen family's scope
 *    the same way -- §5.3), every device in the family (`familyId == fam`),
 *    so admins see devices they neither own nor hold an edge for.
 *
 * `canLocate` on each entry answers "should the 'Locate now' button be
 * enabled": true for an owner or an edge holder always, and for a plain
 * family admin on every family device; false for a super who neither owns
 * the device nor holds an edge (§5.3: "Locate now" hidden for super unless
 * they hold an edge).
 *
 * Shared by `AppShell` (nav item visibility -- "Location" shows once this
 * list is non-empty, or always for a family admin per §5.3) and `/location`
 * (the actual picker), so there is exactly one query for "can I locate
 * anyone" instead of two that could drift.
 */

import { useEffect, useMemo, useState } from "react";
import { collection, onSnapshot, query, where } from "firebase/firestore";

import { useAuth } from "./auth-context";
import { useFamily } from "./family-context";
import { getFirestoreDb } from "./firebase";
import type { DeviceDoc } from "./types";

export interface LocatableDevice extends DeviceDoc {
  id: string;
  /** True when the viewer owns this device (listed as "You"). */
  isOwn: boolean;
  /** True when "Locate now" should be enabled for this device -- see the
   * module docstring. */
  canLocate: boolean;
}

export function useLocatableDevices(): { devices: LocatableDevice[]; loaded: boolean } {
  const { me, isFamilyAdmin, isSuper } = useAuth();
  const { familyId } = useFamily();
  const [granted, setGranted] = useState<LocatableDevice[]>([]);
  const [own, setOwn] = useState<LocatableDevice[]>([]);
  const [familyWide, setFamilyWide] = useState<LocatableDevice[]>([]);
  const [grantedLoaded, setGrantedLoaded] = useState(false);
  const [ownLoaded, setOwnLoaded] = useState(false);
  const [familyLoaded, setFamilyLoaded] = useState(false);

  useEffect(() => {
    if (!me || !familyId) {
      // Signed out, or the family scope hasn't resolved yet (same
      // "leave the last snapshot" shape the module used before family
      // scoping -- harmless, `RequireAuth`/`FamilyProvider` unmount every
      // reader of this hook before that can matter).
      return;
    }
    const devicesRef = collection(getFirestoreDb(), "devices");
    const unsubGranted = onSnapshot(
      query(
        devicesRef,
        where("familyId", "==", familyId),
        where("locatableBy", "array-contains", me.uid)
      ),
      (snap) => {
        setGranted(
          snap.docs.map((d) => ({ id: d.id, ...(d.data() as DeviceDoc), isOwn: false, canLocate: true }))
        );
        setGrantedLoaded(true);
      }
    );
    const unsubOwn = onSnapshot(
      query(devicesRef, where("familyId", "==", familyId), where("ownerUid", "==", me.uid)),
      (snap) => {
        setOwn(
          snap.docs.map((d) => ({ id: d.id, ...(d.data() as DeviceDoc), isOwn: true, canLocate: true }))
        );
        setOwnLoaded(true);
      }
    );
    const unsubFamily = isFamilyAdmin
      ? onSnapshot(query(devicesRef, where("familyId", "==", familyId)), (snap) => {
          setFamilyWide(
            snap.docs.map((d) => ({
              id: d.id,
              ...(d.data() as DeviceDoc),
              isOwn: false,
              // A plain family admin may locate every family device; super
              // needs its own edge or ownership, supplied by the `own`/
              // `granted` listeners above and merged in with priority below.
              canLocate: !isSuper,
            }))
          );
          setFamilyLoaded(true);
        })
      : undefined;
    return () => {
      unsubGranted();
      unsubOwn();
      unsubFamily?.();
    };
  }, [me, familyId, isFamilyAdmin, isSuper]);

  const devices = useMemo(() => {
    const byId = new Map<string, LocatableDevice>();
    for (const d of familyWide) byId.set(d.id, d);
    for (const d of granted) byId.set(d.id, d);
    for (const d of own) byId.set(d.id, d);
    return [...byId.values()];
  }, [own, granted, familyWide]);

  return { devices, loaded: grantedLoaded && ownLoaded && (!isFamilyAdmin || familyLoaded) };
}

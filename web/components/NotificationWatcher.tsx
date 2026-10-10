"use client";

/** Foreground notification rule -- docs/SERVER_PLAN.md §7.6: "the Firestore
 * listener fires; if `document.visibilityState !== 'visible'` or the
 * thread isn't the open one, show a `new Notification(...)`." Mounted once
 * from `AppShell` so it runs on every authenticated page, not just the
 * thread the user happens to have open. Renders nothing.
 *
 * docs/FAMILIES_TASKS.md 2.5: a group conversation used to be announced (and
 * linked) as if it were a DM with an arbitrary other member -- `peerUid` was
 * always `uids.find(u => u !== me.uid)`, which for a group is just whichever
 * member happens to sort first, not a peer at all. A group row now
 * announces "New message in {name}" and links to the group's own
 * `/chat/{alias}` via `showForegroundGroupNotification` below, built
 * locally rather than through `lib/notifications.ts`'s
 * `showForegroundMessageNotification` -- that helper's title is hardcoded
 * to "New message from @{alias}", and this task's file list does not
 * include `lib/notifications.ts`. A DM row's peer alias now prefers
 * `conversation.participants` (docs/FAMILIES_TASKS.md 2.4's per-member
 * alias/name snapshot) over the directory, falling back to
 * `useDirectory().byUid` for a document written before 2.4 lands.
 *
 * docs/FAMILIES_TASKS.md 4.4: a second effect, admin-only, listens to the
 * same `families/{fam}/alerts where status == 'open'` query the badge in
 * `AppShell` counts, and announces any newly-added doc (skipping the
 * snapshot's initial batch, same `initialized` convention as the message
 * effect above) when the tab is hidden or not on `/family/alerts` --
 * docs/FAMILIES_DESIGN.md §5.2, §6 "Foreground".
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import { usePathname } from "next/navigation";
import { useEffect, useRef } from "react";

import { api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import {
  notificationPermission,
  notificationsSupported,
  registerForPush,
  showForegroundMessageNotification,
} from "@/lib/notifications";
import { peerLabel } from "@/lib/names";
import type { AlertDoc, ConversationDoc } from "@/lib/types";

/** Alert notifications have no peer alias/thread to link to -- just the
 * relay-provided `preview` and a click target of `/family/alerts` (docs/
 * FAMILIES_DESIGN.md §6's push payload uses the same `url`). */
function showForegroundAlertNotification(preview: string): void {
  if (notificationPermission() !== "granted") return;
  const n = new Notification("New family alert", {
    body: preview,
    icon: "/icons/icon-192.png",
    tag: "pager-alerts",
  });
  n.onclick = () => {
    window.focus();
    // eslint-disable-next-line @next/next/no-location-assign-relative-destination
    window.location.href = "/family/alerts";
  };
}

function currentThreadAlias(pathname: string | null): string | null {
  if (!pathname) return null;
  const parts = pathname.split("/").filter(Boolean);
  if (parts.length < 2 || parts[0] !== "chat") return null;
  return decodeURIComponent(parts[1]!);
}

/** Same shape as `lib/notifications.ts`'s `showForegroundMessageNotification`,
 * but for a group: its own title and click target, not a peer's alias. See
 * this file's module docstring for why it isn't in that shared module. */
function showForegroundGroupNotification(alias: string, name: string, preview: string): void {
  if (notificationPermission() !== "granted") return;
  const n = new Notification(`New message in ${name}`, {
    body: preview,
    icon: "/icons/icon-192.png",
    tag: `pager-thread-${alias}`,
  });
  n.onclick = () => {
    window.focus();
    // eslint-disable-next-line @next/next/no-location-assign-relative-destination
    window.location.href = `/chat/${encodeURIComponent(alias)}`;
  };
}

export default function NotificationWatcher() {
  const { me, isFamilyAdmin } = useAuth();
  const { byUid } = useDirectory();
  const { familyId } = useFamily();
  const pathname = usePathname();

  const pathnameRef = useRef(pathname);
  useEffect(() => {
    pathnameRef.current = pathname;
  }, [pathname]);

  const byUidRef = useRef(byUid);
  useEffect(() => {
    byUidRef.current = byUid;
  }, [byUid]);

  // Refresh the FCM token once per page load so it never goes stale
  // (docs/SERVER_PLAN.md §7.6: the relay upserts users/{uid}/pushTokens/{token}
  // via POST /api/me/push-tokens). Only when permission is already "granted",
  // so registerForPush()'s requestPermission() call cannot show a prompt.
  const pushRefreshed = useRef(false);
  useEffect(() => {
    if (!me || pushRefreshed.current) return;
    if (!notificationsSupported() || notificationPermission() !== "granted") return;
    pushRefreshed.current = true;
    registerForPush()
      .then((token) => (token ? api.post("/me/push-tokens", { token }) : undefined))
      .catch((err: unknown) => console.warn("push token refresh failed", err));
  }, [me]);

  useEffect(() => {
    if (!me) return;
    const db = getFirestoreDb();
    const q = query(collection(db, "conversations"), where("uids", "array-contains", me.uid));
    const previousUnread = new Map<string, number>();
    let initialized = false;

    const unsubscribe = onSnapshot(q, (snap) => {
      snap.forEach((d) => {
        const data = d.data() as ConversationDoc;
        const unread = data.unread?.[me.uid] ?? 0;
        const prevUnread = previousUnread.get(d.id) ?? 0;

        if (initialized && unread > prevUnread) {
          const openAlias = currentThreadAlias(pathnameRef.current);

          if (data.kind === "group") {
            const alias = data.alias;
            if (alias !== undefined) {
              const threadOpen = alias === openAlias;
              if (document.visibilityState !== "visible" || !threadOpen) {
                showForegroundGroupNotification(alias, data.name ?? alias, data.lastPreview);
              }
            }
          } else {
            const peerUid = data.uids.find((u) => u !== me.uid) ?? data.uids[0]!;
            const part = data.participants?.[peerUid];
            const dir = byUidRef.current(peerUid);
            const alias = part?.alias ?? dir?.alias;
            const title = alias
              ? peerLabel({ alias, displayName: part?.displayName || dir?.displayName, kind: part?.kind ?? dir?.kind })
              : undefined;
            const threadOpen = alias !== undefined && alias === openAlias;
            if (alias !== undefined && (document.visibilityState !== "visible" || !threadOpen)) {
              showForegroundMessageNotification(alias, data.lastPreview, title);
            }
          }
        }
        previousUnread.set(d.id, unread);
      });
      initialized = true;
    });

    return unsubscribe;
  }, [me]);

  useEffect(() => {
    if (!isFamilyAdmin || !familyId) return;
    const db = getFirestoreDb();
    const q = query(
      collection(db, "families", familyId, "alerts"),
      where("status", "==", "open")
    );
    const seen = new Set<string>();
    let initialized = false;

    const unsubscribe = onSnapshot(q, (snap) => {
      snap.forEach((d) => {
        if (initialized && !seen.has(d.id)) {
          const data = d.data() as AlertDoc;
          const onAlertsPage = pathnameRef.current?.startsWith("/family/alerts") ?? false;
          if (document.visibilityState !== "visible" || !onAlertsPage) {
            showForegroundAlertNotification(data.preview);
          }
        }
        seen.add(d.id);
      });
      initialized = true;
    });

    return unsubscribe;
  }, [isFamilyAdmin, familyId]);

  return null;
}

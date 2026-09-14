"use client";

/** Foreground notification rule -- docs/SERVER_PLAN.md §7.6: "the Firestore
 * listener fires; if `document.visibilityState !== 'visible'` or the
 * thread isn't the open one, show a `new Notification(...)`." Mounted once
 * from `AppShell` so it runs on every authenticated page, not just the
 * thread the user happens to have open. Renders nothing.
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import { usePathname } from "next/navigation";
import { useEffect, useRef } from "react";

import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import { showForegroundMessageNotification } from "@/lib/notifications";
import type { ConversationDoc } from "@/lib/types";

function currentThreadAlias(pathname: string | null): string | null {
  if (!pathname) return null;
  const parts = pathname.split("/").filter(Boolean);
  if (parts.length < 2 || parts[0] !== "chat") return null;
  return decodeURIComponent(parts[1]!);
}

export default function NotificationWatcher() {
  const { me } = useAuth();
  const { uidToAlias } = useDirectory();
  const pathname = usePathname();

  const pathnameRef = useRef(pathname);
  useEffect(() => {
    pathnameRef.current = pathname;
  }, [pathname]);

  const uidToAliasRef = useRef(uidToAlias);
  useEffect(() => {
    uidToAliasRef.current = uidToAlias;
  }, [uidToAlias]);

  useEffect(() => {
    if (!me) return;
    const db = getFirestoreDb();
    const q = query(collection(db, "conversations"), where("uids", "array-contains", me.uid));
    const previousUnread = new Map<string, number>();
    let initialized = false;

    const unsubscribe = onSnapshot(q, (snap) => {
      snap.forEach((d) => {
        const data = d.data() as ConversationDoc;
        const peerUid = data.uids.find((u) => u !== me.uid) ?? data.uids[0]!;
        const unread = data.unread?.[me.uid] ?? 0;
        const prevUnread = previousUnread.get(d.id) ?? 0;

        if (initialized && unread > prevUnread) {
          const alias = uidToAliasRef.current(peerUid);
          const openAlias = currentThreadAlias(pathnameRef.current);
          const threadOpen = alias !== undefined && alias === openAlias;
          if (alias !== undefined && (document.visibilityState !== "visible" || !threadOpen)) {
            showForegroundMessageNotification(alias, data.lastPreview);
          }
        }
        previousUnread.set(d.id, unread);
      });
      initialized = true;
    });

    return unsubscribe;
  }, [me]);

  return null;
}

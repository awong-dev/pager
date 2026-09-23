"use client";

/**
 * Best-effort alias <-> uid directory.
 *
 * **Known gap, not a bug**: `firestore.rules`' `users/{uid}` read rule is
 * `request.auth.uid == uid || isAdmin()` (docs/SERVER_PLAN.md §3) -- a
 * regular (non-admin) member can read their *own* `users/{uid}` doc but not
 * a conversation partner's, and there is no other collection or API route
 * (§5.1) that maps a peer's uid to their alias for a non-admin caller. Since
 * `messages`/`conversations` documents are keyed by uid (not alias), and the
 * relay's write endpoints (`POST /api/conversations/{alias}/...`) are keyed
 * by *alias*, a plain member's browser has no server-provided way to learn a
 * new contact's alias before ever messaging them.
 *
 * The real fix is server-side: either loosen `users/{uid}`'s read rule to
 * `registered()` (aliases and display names are not secret -- PROTOCOL.md's
 * own addressing model has devices typing `@alias` in the clear), or add a
 * `GET /api/me/contacts`-shaped endpoint returning `{uid, alias,
 * displayName}` for the caller's conversation/allow-list partners.
 *
 * Workaround implemented below (client-only, no relay/rules change):
 * - An **admin** can list the whole `users` collection (`isAdmin()` doesn't
 *   depend on the resource, so a full collection `list` is provably safe --
 *   the same reasoning `/admin/users` already relies on) -- admins get a
 *   complete, always-current directory for free.
 * - A **member** starts with just themselves resolvable. Opening
 *   `/chat/[alias]` for a brand new contact shows a "starting a new
 *   conversation" composer-only state (no thread listener yet, since the
 *   peer's uid is unknown); the first successful send returns a message id,
 *   which the sender is always allowed to read back (`registered() &&
 *   auth.uid in uids`), revealing `recipientUid`. `learn()` records that
 *   mapping in memory *and* in `localStorage` (key `pager.directory.v1`), so
 *   every contact a member has ever messaged from this browser resolves
 *   instantly on every later visit, even offline.
 */

import {
  type ReactNode,
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
} from "react";
import { collection, onSnapshot, query, where } from "firebase/firestore";

import { useAuth } from "./auth-context";
import { getFirestoreDb } from "./firebase";
import type { ConversationDoc, UserDoc } from "./types";

const STORAGE_KEY = "pager.directory.v1";

interface StoredDirectory {
  uidToAlias: Record<string, string>;
}

function loadStored(): StoredDirectory {
  if (typeof window === "undefined") {
    return { uidToAlias: {} };
  }
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    if (!raw) return { uidToAlias: {} };
    const parsed = JSON.parse(raw) as StoredDirectory;
    return { uidToAlias: parsed.uidToAlias ?? {} };
  } catch {
    return { uidToAlias: {} };
  }
}

function saveStored(data: StoredDirectory): void {
  if (typeof window === "undefined") return;
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(data));
  } catch {
    // Storage full/unavailable (private browsing) -- the directory just
    // stays memory-only for this session, not worth surfacing to the user.
  }
}

// docs/GROUP_CHAT_DESIGN.md §5: the "New group" dialog's checkbox list of
// existing contacts. Populated by the same admin-only `users` snapshot as
// `uidToAliasMap` below, just kept in list (not map) form -- only ever
// non-empty for an admin, same gate as `isComplete`.
export interface Contact {
  uid: string;
  alias: string;
  displayName: string;
}

// docs/GROUP_CHAT_DESIGN.md §5: a group conversation this member belongs
// to, keyed by the group's own `alias` field so `ThreadPageClient.tsx` can
// resolve `/chat/[alias]` to a `convKey` without a new query or index (the
// listener below is the same `uids array-contains me` shape
// `web/app/chat/page.tsx` already runs and `firestore.rules` already
// allows).
export interface GroupInfo {
  convKey: string;
  alias: string;
  name: string;
  uids: string[];
}

interface DirectoryContextValue {
  /** `undefined` if this alias has never been resolved by this browser. */
  aliasToUid: (alias: string) => string | undefined;
  uidToAlias: (uid: string) => string | undefined;
  /** Records a newly-discovered (uid, alias) pair, in memory and locally. */
  learn: (uid: string, alias: string) => void;
  /** True once an admin's full-directory snapshot has loaded at least once. */
  isComplete: boolean;
  /** Admin-only; empty for a member (see `Contact`'s docstring). */
  contacts: Contact[];
  /** `undefined` if `alias` names no group this member belongs to (either
   * it's a DM peer's alias, or a group this account isn't a member of --
   * indistinguishable from here, same as `aliasToUid`'s `undefined`). */
  groupByAlias: (alias: string) => GroupInfo | undefined;
}

const DirectoryContext = createContext<DirectoryContextValue | null>(null);

export function DirectoryProvider({ children }: { children: ReactNode }) {
  const { isAdmin, me } = useAuth();
  // Lazy initializer (runs once, on first mount only) rather than a mount
  // effect + setState -- avoids a synchronous setState-in-effect and the
  // extra empty-map render that would come before it.
  const [uidToAliasMap, setUidToAliasMap] = useState<Map<string, string>>(
    () => new Map(Object.entries(loadStored().uidToAlias))
  );
  const [isComplete, setIsComplete] = useState(false);
  const [contacts, setContacts] = useState<Contact[]>([]);
  const [groupsByAlias, setGroupsByAlias] = useState<Map<string, GroupInfo>>(new Map());

  // Admins get the whole directory live; a member's map stays whatever
  // localStorage + `learn()` have accumulated (self is folded in below at
  // read time, not written into this map, so no effect is needed just to
  // resolve one's own uid).
  useEffect(() => {
    if (!isAdmin) {
      return;
    }
    const unsubscribe = onSnapshot(collection(getFirestoreDb(), "users"), (snap) => {
      const nextContacts: Contact[] = [];
      setUidToAliasMap((prev) => {
        const next = new Map(prev);
        snap.forEach((doc) => {
          const data = doc.data() as UserDoc;
          next.set(doc.id, data.alias);
          nextContacts.push({ uid: doc.id, alias: data.alias, displayName: data.displayName });
        });
        return next;
      });
      setContacts(nextContacts);
      setIsComplete(true);
    });
    return unsubscribe;
  }, [isAdmin]);

  // Group conversations this member belongs to -- every signed-in account,
  // not just admins (membership, not admin status, is what scopes a group
  // read; `firestore.rules` agrees). A DM's `conversations` doc has no
  // `alias` field and is simply skipped.
  useEffect(() => {
    if (!me) {
      // Signed out: leave whatever the map already held rather than
      // setState-ing synchronously in the effect body (the same
      // `react-hooks/set-state-in-effect` constraint the admin-directory
      // effect above sidesteps by never resetting on `!isAdmin` either) --
      // harmless, since nothing reads this map while signed out.
      return;
    }
    const q = query(collection(getFirestoreDb(), "conversations"), where("uids", "array-contains", me.uid));
    const unsubscribe = onSnapshot(q, (snap) => {
      const next = new Map<string, GroupInfo>();
      snap.forEach((d) => {
        const data = d.data() as ConversationDoc;
        if (data.kind === "group" && data.alias) {
          next.set(data.alias, {
            convKey: d.id,
            alias: data.alias,
            name: data.name ?? data.alias,
            uids: data.uids,
          });
        }
      });
      setGroupsByAlias(next);
    });
    return unsubscribe;
  }, [me]);

  const learn = useCallback((uid: string, alias: string) => {
    setUidToAliasMap((prev) => {
      if (prev.get(uid) === alias) return prev;
      const next = new Map(prev);
      next.set(uid, alias);
      saveStored({ uidToAlias: Object.fromEntries(next) });
      return next;
    });
  }, []);

  const aliasToUidMap = useMemo(() => {
    const inverse = new Map<string, string>();
    for (const [uid, alias] of uidToAliasMap) {
      inverse.set(alias, uid);
    }
    if (me) inverse.set(me.alias, me.uid);
    return inverse;
  }, [uidToAliasMap, me]);

  const value: DirectoryContextValue = {
    aliasToUid: (alias) => aliasToUidMap.get(alias),
    uidToAlias: (uid) => (me && uid === me.uid ? me.alias : uidToAliasMap.get(uid)),
    learn,
    isComplete,
    contacts,
    groupByAlias: (alias) => groupsByAlias.get(alias),
  };

  return <DirectoryContext.Provider value={value}>{children}</DirectoryContext.Provider>;
}

export function useDirectory(): DirectoryContextValue {
  const ctx = useContext(DirectoryContext);
  if (!ctx) {
    throw new Error("useDirectory must be used within DirectoryProvider");
  }
  return ctx;
}

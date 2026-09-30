"use client";

/**
 * Family-scoped directory -- docs/FAMILIES_DESIGN.md §5.1 `useDirectory()`,
 * docs/FAMILIES_TASKS.md 1.9.
 *
 * Two sources merged into one uid-keyed map:
 * - a live Firestore listener on `users where familyId == useFamily().familyId`
 *   (`firestore.rules`'s `sameFam()` predicate, task 1.4, lets any member of
 *   a family list the whole family, not just an admin) -- always fresh for
 *   the caller's own family;
 * - `GET /api/directory` (task 1.5), fetched once per sign-in and again on
 *   every family switch (super only -- `familyId` never changes for anyone
 *   else) -- the caller's family plus every `allow`-edge peer (possibly in
 *   another family) and every external they share a conversation with.
 *
 * This replaces the old client-only workaround: an admin-only full `users`
 * listener plus a per-browser `localStorage` cache seeded by `learn()` on a
 * contact's first send (see git history for that version's long docstring).
 * Every signed-in member now has a server-provided way to resolve a peer's
 * alias, so that cache is gone. `learn()` is kept as a documented no-op so
 * call sites this task does not touch (`web/app/chat/[alias]/
 * ThreadPageClient.tsx`) keep compiling.
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

import { api } from "./api";
import { useAuth } from "./auth-context";
import { useFamily } from "./family-context";
import { getFirestoreDb } from "./firebase";
import type { ConversationDoc, Role, UserDoc, UserKind } from "./types";

// docs/FAMILIES_DESIGN.md §4: `GET /api/directory`'s entry shape, also used
// (minus `phone`) for the rows read straight off the `users` listener.
export interface DirectoryEntry {
  uid: string;
  alias: string;
  displayName: string;
  kind: UserKind;
  familyId: string | null;
  role: Role;
  phone?: string;
}

// docs/GROUP_CHAT_DESIGN.md §5: the "New group" dialog's checkbox list of
// existing contacts -- a narrower shape than `DirectoryEntry`, kept as its
// own type since that's all `NewGroupDialog.tsx` (outside this task) needs.
export interface Contact {
  uid: string;
  alias: string;
  displayName: string;
}

// docs/GROUP_CHAT_DESIGN.md §5: a group conversation this member belongs
// to, keyed by the group's own `alias` field so `ThreadPageClient.tsx` can
// resolve `/chat/[alias]` to a `convKey` without a new query or index.
export interface GroupInfo {
  convKey: string;
  alias: string;
  name: string;
  uids: string[];
}

interface DirectoryContextValue {
  /** `undefined` if `uid` has not been resolved by either source below. */
  byUid: (uid: string) => DirectoryEntry | undefined;
  /** Same lookup, keyed by `alias`. */
  byAlias: (alias: string) => DirectoryEntry | undefined;
  /** @deprecated alias for `byAlias(alias)?.uid`, kept for call sites this
   * task does not touch (docs/FAMILIES_TASKS.md 1.9). */
  aliasToUid: (alias: string) => string | undefined;
  /** @deprecated alias for `byUid(uid)?.alias`, same as above. */
  uidToAlias: (uid: string) => string | undefined;
  /** @deprecated no-op -- the directory is now server-provided and live, so
   * there is nothing left to "learn" client-side. Kept so
   * `ThreadPageClient.tsx`'s post-send call keeps compiling. */
  learn: (uid: string, alias: string) => void;
  /** True once the family listener and the one-shot `/api/directory` fetch
   * have both resolved at least once for the family currently in scope. */
  isComplete: boolean;
  /** Every resolved person (family members plus allow-edge/conversation
   * peers, in or out of family), including self. */
  contacts: Contact[];
  /** Every resolved external (SMS) contact. */
  externals: DirectoryEntry[];
  groupByAlias: (alias: string) => GroupInfo | undefined;
}

const DirectoryContext = createContext<DirectoryContextValue | null>(null);

interface ApiDirectoryResponse {
  entries: DirectoryEntry[];
}

export function DirectoryProvider({ children }: { children: ReactNode }) {
  const { me } = useAuth();
  const { familyId } = useFamily();
  const [familyEntries, setFamilyEntries] = useState<Map<string, DirectoryEntry>>(new Map());
  const [familyLoaded, setFamilyLoaded] = useState(false);
  const [apiEntries, setApiEntries] = useState<Map<string, DirectoryEntry>>(new Map());
  const [apiLoaded, setApiLoaded] = useState(false);
  const [groupsByAlias, setGroupsByAlias] = useState<Map<string, GroupInfo>>(new Map());

  // Live: every member of the family in scope. `firestore.rules` (task 1.4)
  // lets any `sameFam()` member list this query, not just an admin.
  useEffect(() => {
    if (!familyId) {
      // No family in scope yet (a super who hasn't picked one, or the
      // instant after sign-in before `/api/me` resolves) -- leave whatever
      // the map already held rather than clearing synchronously in the
      // effect body (same `react-hooks/set-state-in-effect` constraint
      // `lib/family-context.tsx`'s admin-only `families` listener sidesteps
      // by never resetting on `!isSuper` either) -- harmless, since a
      // stale/empty map from a different scope is replaced the moment this
      // effect's own listener below fires for the new scope.
      return;
    }
    const q = query(collection(getFirestoreDb(), "users"), where("familyId", "==", familyId));
    const unsubscribe = onSnapshot(q, (snap) => {
      const next = new Map<string, DirectoryEntry>();
      snap.forEach((doc) => {
        const data = doc.data() as UserDoc;
        next.set(doc.id, {
          uid: doc.id,
          alias: data.alias,
          displayName: data.displayName,
          kind: data.kind,
          familyId: data.familyId,
          role: data.role,
        });
      });
      setFamilyEntries(next);
      setFamilyLoaded(true);
    });
    return unsubscribe;
  }, [familyId]);

  // One-shot: cross-family peers and externals -- refetched on sign-in and
  // on every family switch (super only; `familyId` is otherwise fixed, so
  // this only ever runs once per sign-in for an admin/member).
  useEffect(() => {
    if (!me) {
      // Signed out: leave whatever the map already held, same convention
      // as the family listener above -- harmless, since nothing reads this
      // map while signed out.
      return;
    }
    let cancelled = false;
    (async () => {
      try {
        const resp = await api.get<ApiDirectoryResponse>("/directory");
        if (cancelled) return;
        const next = new Map<string, DirectoryEntry>();
        for (const entry of resp.entries) {
          next.set(entry.uid, entry);
        }
        setApiEntries(next);
      } catch {
        // Transient (offline, relay not up yet in dev) -- the family
        // listener above still resolves same-family peers; cross-family and
        // external resolution just stays whatever it last was.
      } finally {
        if (!cancelled) setApiLoaded(true);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [me, familyId]);

  // Group conversations this member belongs to -- every signed-in account,
  // unaffected by the directory rewrite above. A DM's `conversations` doc
  // has no `alias` field and is simply skipped.
  useEffect(() => {
    if (!me) {
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

  // Merge: the live family listener wins over the one-shot API fetch for
  // any uid both name (freshest alias/displayName/role for the family in
  // scope); the API fetch is the only source for cross-family peers and
  // externals. Self is always resolvable from `me`, even before either
  // source above has loaded.
  const byUidMap = useMemo(() => {
    const merged = new Map<string, DirectoryEntry>(apiEntries);
    for (const [uid, entry] of familyEntries) {
      merged.set(uid, entry);
    }
    if (me) {
      const existing = merged.get(me.uid);
      merged.set(me.uid, {
        uid: me.uid,
        alias: me.alias,
        displayName: me.displayName,
        kind: me.kind,
        familyId: me.familyId,
        role: me.role,
        phone: existing?.phone,
      });
    }
    return merged;
  }, [apiEntries, familyEntries, me]);

  const byAliasMap = useMemo(() => {
    const inverse = new Map<string, DirectoryEntry>();
    for (const entry of byUidMap.values()) {
      inverse.set(entry.alias, entry);
    }
    return inverse;
  }, [byUidMap]);

  // No-op -- see module docstring. Declared with no parameters (still
  // assignable to `(uid: string, alias: string) => void`) so there is
  // nothing to mark unused.
  const learn = useCallback(() => {}, []);

  const contacts = useMemo<Contact[]>(
    () =>
      Array.from(byUidMap.values())
        .filter((e) => e.kind === "person")
        .map((e) => ({ uid: e.uid, alias: e.alias, displayName: e.displayName })),
    [byUidMap]
  );

  const externals = useMemo(
    () => Array.from(byUidMap.values()).filter((e) => e.kind === "external"),
    [byUidMap]
  );

  const value: DirectoryContextValue = {
    byUid: (uid) => byUidMap.get(uid),
    byAlias: (alias) => byAliasMap.get(alias),
    aliasToUid: (alias) => byAliasMap.get(alias)?.uid,
    uidToAlias: (uid) => byUidMap.get(uid)?.alias,
    learn,
    isComplete: familyLoaded && apiLoaded,
    contacts,
    externals,
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

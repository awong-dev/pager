"use client";

/**
 * Family scope -- docs/FAMILIES_DESIGN.md §1 decision 8, §5.1.
 *
 * For an `admin`/`member`, the scope is fixed to `me.familyId`: they have
 * exactly one family and no switcher. For `super`, the scope is chosen from
 * every `families/{fid}` doc (`firestore.rules` lets super read the whole
 * collection), read once from `?family=` on first load, else from
 * `sessionStorage['pager.family']`, else `me.familyId`; picking a family
 * persists it to `sessionStorage` for the rest of the tab's life.
 *
 * `familyQuery()` is a plain (non-hook) helper for `lib/api.ts` call sites
 * that need `?family=<fid>` appended to a `/api/family/*` call --
 * `lib/api.ts` is a plain module, not a component, so it cannot call
 * `useFamily()` itself. It mirrors whatever `FamilyProvider` last resolved
 * via a module-level variable kept in sync by an effect below.
 *
 * The `?family=` query param is read the same way `app/location/page.tsx`
 * reads its own `?who=` -- a lazy `useState` initializer off
 * `window.location.search`, not `useSearchParams()` -- avoiding that hook's
 * Suspense-boundary requirement under `output: 'export'` for one optional
 * parameter.
 */

import {
  type ReactNode,
  createContext,
  useContext,
  useEffect,
  useMemo,
  useState,
} from "react";
import { collection, onSnapshot } from "firebase/firestore";

import { useAuth } from "./auth-context";
import { getFirestoreDb } from "./firebase";
import type { FamilyDoc } from "./types";

const STORAGE_KEY = "pager.family";

export interface FamilySummary {
  id: string;
  name: string;
}

interface FamilyContextValue {
  familyId: string | null;
  family: FamilySummary | null;
  setFamilyId: (id: string) => void;
  families: FamilySummary[];
  /** True for `super` -- whether a switcher should be shown at all. */
  canSwitch: boolean;
}

const FamilyContext = createContext<FamilyContextValue | null>(null);

// Mirrors the provider's resolved scope for `familyQuery()`, a plain
// function `lib/api.ts` can call outside of React. Module-scoped, so it
// resets to the default on a full reload just like `sessionStorage` does.
let currentScope: { familyId: string | null; canSwitch: boolean } = {
  familyId: null,
  canSwitch: false,
};

/** `?family=<fid>` for a super with a family selected, `''` for everyone
 * else (an admin/member's calls are already scoped server-side from their
 * token's `fam` claim -- docs/FAMILIES_DESIGN.md §4). Append this to any
 * `/api/family/*` call. */
export function familyQuery(): string {
  if (!currentScope.canSwitch || !currentScope.familyId) return "";
  return `?family=${encodeURIComponent(currentScope.familyId)}`;
}

function readFamilyParam(): string | null {
  if (typeof window === "undefined") return null;
  return new URLSearchParams(window.location.search).get("family");
}

function readStoredFamily(): string | null {
  if (typeof window === "undefined") return null;
  try {
    return window.sessionStorage.getItem(STORAGE_KEY);
  } catch {
    return null;
  }
}

export function FamilyProvider({ children }: { children: ReactNode }) {
  const { me, isSuper } = useAuth();
  const [families, setFamilies] = useState<FamilySummary[]>([]);
  // Lazy initializer -- runs once, mirroring `?family=` / sessionStorage at
  // mount, not on every render.
  const [selected, setSelected] = useState<string | null>(
    () => readFamilyParam() ?? readStoredFamily()
  );

  // Super only: `families` live, for the switcher and for resolving a
  // family's display name. Never resets on `!isSuper` (same
  // set-state-in-effect constraint `lib/directory.tsx`'s admin-directory
  // effect sidesteps the same way) -- harmless, since `canSwitch` below
  // hides the switcher and nothing else reads `families` for a non-super.
  useEffect(() => {
    if (!isSuper) {
      return;
    }
    const unsubscribe = onSnapshot(collection(getFirestoreDb(), "families"), (snap) => {
      setFamilies(
        snap.docs
          .map((d) => ({ id: d.id, name: (d.data() as FamilyDoc).name }))
          .sort((a, b) => a.name.localeCompare(b.name))
      );
    });
    return unsubscribe;
  }, [isSuper]);

  const familyId = isSuper ? (selected ?? me?.familyId ?? null) : (me?.familyId ?? null);

  // Keep the module-level mirror in sync for `familyQuery()`.
  useEffect(() => {
    currentScope = { familyId, canSwitch: isSuper };
  }, [familyId, isSuper]);

  function setFamilyId(id: string) {
    setSelected(id);
    if (typeof window !== "undefined") {
      try {
        window.sessionStorage.setItem(STORAGE_KEY, id);
      } catch {
        // Storage full/unavailable (private browsing) -- the choice just
        // stays memory-only for this render tree, not worth surfacing.
      }
    }
  }

  const family = useMemo(
    () => families.find((f) => f.id === familyId) ?? null,
    [families, familyId]
  );

  const value: FamilyContextValue = {
    familyId,
    family,
    setFamilyId,
    families,
    canSwitch: isSuper,
  };

  return <FamilyContext.Provider value={value}>{children}</FamilyContext.Provider>;
}

export function useFamily(): FamilyContextValue {
  const ctx = useContext(FamilyContext);
  if (!ctx) {
    throw new Error("useFamily must be used within FamilyProvider");
  }
  return ctx;
}

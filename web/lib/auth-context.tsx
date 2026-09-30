"use client";

/**
 * Auth state -- docs/SERVER_PLAN.md §7.3, docs/FAMILIES_DESIGN.md §5.1.
 * Wraps Firebase Auth's `onAuthStateChanged` and, on every sign-in, calls
 * `GET /api/me` to enforce the registry gate client-side (the relay/rules
 * enforce it for real; this is UX only, per the build brief: "the
 * client-side hiding is UX only, not the security boundary"). A 403 here
 * means "signed in to Firebase but not in `users/{uid}`" -- shown as "ask
 * your admin to add you" and signed out, exactly as §7.3 specifies.
 *
 * Multi-family fields (`familyId`, `kind`, `policy`, `notify`) are read
 * defensively: the relay side of the family cutover (docs/FAMILIES_TASKS.md
 * 1.1-1.6) may not be deployed yet, so a response missing them is treated as
 * a legacy single-family user (`familyId: null`, `kind: 'person'`, default
 * policy, alerts on) rather than a crash. `claimsStale: true` means the
 * token's `role`/`fam` custom claims disagree with the user doc (the server
 * just rewrote them) -- force one token refresh and refetch `/api/me` once.
 */

import {
  type User as FirebaseUser,
  onAuthStateChanged,
  signOut as firebaseSignOut,
} from "firebase/auth";
import {
  type ReactNode,
  createContext,
  useCallback,
  useContext,
  useEffect,
  useState,
} from "react";

import { ApiError, api } from "./api";
import { getFirebaseAuth } from "./firebase";
import type { PolicyDoc, Role, UserKind } from "./types";

export interface Me {
  uid: string;
  alias: string;
  displayName: string;
  role: Role;
  familyId: string | null;
  kind: UserKind;
  policy: PolicyDoc;
  notify: { alerts: boolean };
}

// The raw `/api/me` shape: only the fields a pre-cutover relay is guaranteed
// to send are required; the rest are normalised by `normalizeMe` below.
type MeResponse = Partial<Omit<Me, "uid" | "alias" | "displayName" | "role">> &
  Pick<Me, "uid" | "alias" | "displayName" | "role">;

function normalizeMe(user: MeResponse): Me {
  return {
    uid: user.uid,
    alias: user.alias,
    displayName: user.displayName,
    role: user.role,
    familyId: user.familyId ?? null,
    kind: user.kind ?? "person",
    policy: user.policy ?? { out: "people", in: "people" },
    notify: user.notify ?? { alerts: true },
  };
}

export type AuthStatus = "loading" | "signed-out" | "not-registered" | "signed-in";

interface AuthContextValue {
  status: AuthStatus;
  firebaseUser: FirebaseUser | null;
  me: Me | null;
  /** `role === 'super'` -- the one global admin. */
  isSuper: boolean;
  /** `role === 'admin' || isSuper` -- a family admin (super included: super
   * can always do what a family admin can, scoped by `useFamily()`). */
  isFamilyAdmin: boolean;
  /** @deprecated alias for `isFamilyAdmin`, kept for call sites this task
   * does not touch (`lib/directory.tsx`, `app/chat/page.tsx`,
   * `components/NewGroupDialog.tsx`) -- docs/FAMILIES_TASKS.md 1.7. */
  isAdmin: boolean;
  notRegisteredMessage: string | null;
  signOutUser: () => Promise<void>;
}

const AuthContext = createContext<AuthContextValue | null>(null);

const NOT_REGISTERED_MESSAGE =
  "This account is not registered with the pager. Ask your admin to add you.";

export function AuthProvider({ children }: { children: ReactNode }) {
  const [status, setStatus] = useState<AuthStatus>("loading");
  const [firebaseUser, setFirebaseUser] = useState<FirebaseUser | null>(null);
  const [me, setMe] = useState<Me | null>(null);

  useEffect(() => {
    const auth = getFirebaseAuth();
    const unsubscribe = onAuthStateChanged(auth, async (user) => {
      setFirebaseUser(user);
      if (!user) {
        setMe(null);
        setStatus("signed-out");
        return;
      }
      setStatus("loading");
      try {
        let resp = await api.get<{ user: MeResponse; role: Role; claimsStale?: boolean }>(
          "/me"
        );
        if (resp.claimsStale) {
          // The server just rewrote this account's custom claims (role/fam
          // changed) -- force a token refresh so the next relay/rules call
          // sees them, then refetch once (not a loop: the doc and the
          // refreshed claims now agree).
          await user.getIdToken(true);
          resp = await api.get<{ user: MeResponse; role: Role; claimsStale?: boolean }>("/me");
        }
        setMe(normalizeMe(resp.user));
        setStatus("signed-in");
      } catch (err) {
        setMe(null);
        if (err instanceof ApiError && err.status === 403) {
          setStatus("not-registered");
          await firebaseSignOut(auth);
        } else {
          // A transient failure (network, emulator not up yet) -- treat as
          // signed-out rather than silently wedging the UI in "loading".
          setStatus("signed-out");
        }
      }
    });
    return unsubscribe;
  }, []);

  const signOutUser = useCallback(async () => {
    await firebaseSignOut(getFirebaseAuth());
    setMe(null);
    setStatus("signed-out");
  }, []);

  const isSuper = me?.role === "super";
  const isFamilyAdmin = me?.role === "admin" || isSuper;

  const value: AuthContextValue = {
    status,
    firebaseUser,
    me,
    isSuper,
    isFamilyAdmin,
    isAdmin: isFamilyAdmin,
    notRegisteredMessage: status === "not-registered" ? NOT_REGISTERED_MESSAGE : null,
    signOutUser,
  };

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuth(): AuthContextValue {
  const ctx = useContext(AuthContext);
  if (!ctx) {
    throw new Error("useAuth must be used within AuthProvider");
  }
  return ctx;
}

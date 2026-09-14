"use client";

/**
 * Auth state -- docs/SERVER_PLAN.md §7.3. Wraps Firebase Auth's
 * `onAuthStateChanged` and, on every sign-in, calls `GET /api/me` to enforce
 * the registry gate client-side (the relay/rules enforce it for real; this
 * is UX only, per the build brief: "the client-side hiding is UX only, not
 * the security boundary"). A 403 here means "signed in to Firebase but not
 * in `users/{uid}`" -- shown as "ask your admin to add you" and signed out,
 * exactly as §7.3 specifies.
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
import type { Role } from "./types";

export interface Me {
  uid: string;
  alias: string;
  displayName: string;
  role: Role;
}

export type AuthStatus = "loading" | "signed-out" | "not-registered" | "signed-in";

interface AuthContextValue {
  status: AuthStatus;
  firebaseUser: FirebaseUser | null;
  me: Me | null;
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
        const resp = await api.get<{ user: Me; role: Role }>("/me");
        setMe(resp.user);
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

  const value: AuthContextValue = {
    status,
    firebaseUser,
    me,
    isAdmin: me?.role === "admin",
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

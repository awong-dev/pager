/**
 * Firebase JS SDK init -- docs/SERVER_PLAN.md §7.1. One app instance, shared
 * Auth/Firestore/Messaging handles. Emulator-aware: `NEXT_PUBLIC_USE_EMULATORS`
 * gates `connectAuthEmulator`/`connectFirestoreEmulator`, matching the
 * relay's own `FIRESTORE_EMULATOR_HOST`/`FIREBASE_AUTH_EMULATOR_HOST` env-var
 * gate (relay/app/db/firestore.py). Never touches a real Firebase project in
 * this repo -- `NEXT_PUBLIC_FIREBASE_*` are dummy/demo values for local dev
 * (see web/.env.local.example) and are only meaningful for real once a
 * deployment fills in a real project's config (out of scope here, per the
 * build brief: "Never sign up for a real Firebase project").
 */

import { type FirebaseApp, getApps, initializeApp } from "firebase/app";
import {
  type Auth,
  connectAuthEmulator,
  getAuth,
} from "firebase/auth";
import {
  type Firestore,
  connectFirestoreEmulator,
  getFirestore,
} from "firebase/firestore";

// `||`, not `??` -- confirmed live (deploy.yml sets these from GitHub Actions
// repo variables that hadn't actually been created yet): an unset
// `vars.FOO` in a workflow expands to an empty string, not nothing, so
// Next.js inlines `process.env.NEXT_PUBLIC_FIREBASE_API_KEY` as `""` at
// build time -- `??` only falls back on null/undefined, so `""` sailed
// straight through as `firebaseConfig.apiKey`, and Firebase Auth's
// "auth/invalid-api-key" (a blank/malformed key) is a strictly worse,
// harder-to-diagnose failure than "auth/api-key-not-valid" (an obviously-a-
// placeholder key) would have been. `||` treats "" the same as unset.
const firebaseConfig = {
  apiKey: process.env.NEXT_PUBLIC_FIREBASE_API_KEY || "demo-api-key",
  authDomain: process.env.NEXT_PUBLIC_FIREBASE_AUTH_DOMAIN || "demo-pager.firebaseapp.com",
  projectId: process.env.NEXT_PUBLIC_FIREBASE_PROJECT_ID || "demo-pager",
  storageBucket: process.env.NEXT_PUBLIC_FIREBASE_STORAGE_BUCKET || "demo-pager.appspot.com",
  messagingSenderId: process.env.NEXT_PUBLIC_FIREBASE_MESSAGING_SENDER_ID || "0",
  appId: process.env.NEXT_PUBLIC_FIREBASE_APP_ID || "1:0:web:0",
};

const USE_EMULATORS = process.env.NEXT_PUBLIC_USE_EMULATORS === "1";
const FIRESTORE_EMULATOR_HOST = process.env.NEXT_PUBLIC_FIRESTORE_EMULATOR_HOST ?? "localhost";
const FIRESTORE_EMULATOR_PORT = Number(
  process.env.NEXT_PUBLIC_FIRESTORE_EMULATOR_PORT ?? "8080"
);
const AUTH_EMULATOR_URL =
  process.env.NEXT_PUBLIC_AUTH_EMULATOR_URL ?? "http://localhost:9099";

let app: FirebaseApp | undefined;
let auth: Auth | undefined;
let db: Firestore | undefined;

export function getFirebaseApp(): FirebaseApp {
  if (!app) {
    app = getApps().length ? getApps()[0]! : initializeApp(firebaseConfig);
  }
  return app;
}

export function getFirebaseAuth(): Auth {
  if (!auth) {
    auth = getAuth(getFirebaseApp());
    if (USE_EMULATORS) {
      // `connectAuthEmulator`/`connectFirestoreEmulator` throw if called
      // twice on the same instance, which Next's fast-refresh in dev can
      // otherwise trigger -- guarded by the `!auth`/`!db` checks above
      // rather than a second flag, so this only ever runs once per
      // module instance.
      connectAuthEmulator(auth, AUTH_EMULATOR_URL, { disableWarnings: true });
    }
  }
  return auth;
}

export function getFirestoreDb(): Firestore {
  if (!db) {
    db = getFirestore(getFirebaseApp());
    if (USE_EMULATORS) {
      connectFirestoreEmulator(db, FIRESTORE_EMULATOR_HOST, FIRESTORE_EMULATOR_PORT);
    }
  }
  return db;
}

export { USE_EMULATORS };

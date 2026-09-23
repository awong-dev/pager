#!/usr/bin/env node
// Prebuild step -- docs/V03_PLAN.md §3a / docs/V03_TASKS.md 3a.2.
//
// Reads the same `NEXT_PUBLIC_FIREBASE_*` env vars `next build` inlines into
// the app (via `.env.local`/`.env.production`/etc., loaded the same way Next
// itself loads them -- `@next/env` is a real dependency of `next`, not an
// extra one added here) and substitutes them into
// `web/sw/firebase-messaging-sw.template.js`, writing the result to
// `web/public/firebase-messaging-sw.js`. That generated file is
// `.gitignore`d: a service worker can't go through webpack/Next's bundler
// (it's fetched byte-for-byte from `/`), so there is no other way to keep it
// off demo/placeholder config short of checking in real project secrets --
// and Firebase web config isn't secret, but it does need to be *real* for
// push to work at all. Unlike `lib/firebase.ts` (which falls back to a
// "demo-pager" config so the app still builds/runs against the emulator
// stack with no `.env.local` at all), this script never falls back: a
// missing value fails the build loudly instead of silently shipping a
// service worker that can never receive a push.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
// `@next/env` is published as CommonJS; Node's ESM loader only gives us the
// default export, not the named ones `next` itself gets via `require()`.
import nextEnv from "@next/env";
const { loadEnvConfig } = nextEnv;

const webDir = join(dirname(fileURLToPath(import.meta.url)), "..");

// Same loader `next dev`/`next build` use internally, so `.env.local` /
// `.env.production.local` / etc. precedence matches the app exactly.
loadEnvConfig(webDir, process.env.NODE_ENV !== "production");

// The six values the compat SDK's `firebase.initializeApp()` needs. Order
// matters only for the error message below.
const REQUIRED_VARS = [
  "NEXT_PUBLIC_FIREBASE_API_KEY",
  "NEXT_PUBLIC_FIREBASE_AUTH_DOMAIN",
  "NEXT_PUBLIC_FIREBASE_PROJECT_ID",
  "NEXT_PUBLIC_FIREBASE_STORAGE_BUCKET",
  "NEXT_PUBLIC_FIREBASE_MESSAGING_SENDER_ID",
  "NEXT_PUBLIC_FIREBASE_APP_ID",
];

const missing = REQUIRED_VARS.filter((name) => !process.env[name]);
if (missing.length > 0) {
  console.error(
    `gen-sw: missing required env var(s) for public/firebase-messaging-sw.js: ${missing.join(
      ", "
    )}. Copy web/.env.local.example to web/.env.local (or set these in the ` +
      "build environment) -- no demo/placeholder config is ever baked into the " +
      "generated service worker."
  );
  process.exit(1);
}

const templatePath = join(webDir, "sw", "firebase-messaging-sw.template.js");
let template = readFileSync(templatePath, "utf8");

for (const name of REQUIRED_VARS) {
  const placeholder = `__${name}__`;
  if (!template.includes(placeholder)) {
    console.error(`gen-sw: template is missing placeholder ${placeholder}`);
    process.exit(1);
  }
  template = template.split(placeholder).join(process.env[name]);
}

const outDir = join(webDir, "public");
mkdirSync(outDir, { recursive: true });
const outPath = join(outDir, "firebase-messaging-sw.js");
writeFileSync(outPath, template);
console.log(`gen-sw: wrote ${outPath}`);

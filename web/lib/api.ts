/**
 * Thin `fetch` wrapper for every relay write -- docs/SERVER_PLAN.md §5.1:
 * "every write goes through here" (the relay API), never a direct Firestore
 * write from the browser. Adds `Authorization: Bearer <Firebase ID token>`;
 * everything else (reads) is a Firestore listener (see lib/firebase.ts).
 *
 * Base path is always `/api` (relative): `next.config.js` rewrites it to
 * `http://localhost:8000` in dev, and Firebase Hosting rewrites it to the
 * Cloud Run relay in prod (web/firebase.json) -- the web app itself never
 * hardcodes a relay origin.
 */

import { getFirebaseAuth } from "./firebase";

export class ApiError extends Error {
  status: number;
  detail: unknown;

  constructor(status: number, detail: unknown) {
    super(typeof detail === "string" ? detail : `request failed with status ${status}`);
    this.status = status;
    this.detail = detail;
  }
}

async function authHeader(): Promise<Record<string, string>> {
  const user = getFirebaseAuth().currentUser;
  if (!user) {
    throw new ApiError(401, "not signed in");
  }
  const token = await user.getIdToken();
  return { Authorization: `Bearer ${token}` };
}

async function request<T>(
  method: "GET" | "POST" | "PATCH" | "PUT" | "DELETE",
  path: string,
  body?: unknown
): Promise<T> {
  const headers: Record<string, string> = { ...(await authHeader()) };
  let payload: string | undefined;
  if (body !== undefined) {
    headers["Content-Type"] = "application/json";
    payload = JSON.stringify(body);
  }
  const resp = await fetch(`/api${path}`, { method, headers, body: payload });
  const text = await resp.text();
  const data = text ? JSON.parse(text) : undefined;
  if (!resp.ok) {
    const detail = data && typeof data === "object" && "detail" in data ? data.detail : data;
    throw new ApiError(resp.status, detail);
  }
  return data as T;
}

export const api = {
  get: <T>(path: string) => request<T>("GET", path),
  post: <T>(path: string, body?: unknown) => request<T>("POST", path, body ?? {}),
  patch: <T>(path: string, body?: unknown) => request<T>("PATCH", path, body ?? {}),
  put: <T>(path: string, body?: unknown) => request<T>("PUT", path, body ?? {}),
  del: <T>(path: string) => request<T>("DELETE", path),
};

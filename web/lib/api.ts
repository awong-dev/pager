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

// docs/FAMILIES_DESIGN.md §4: a policy-gated write's 403 body carries
// `{reason, message}` -- `reason` a machine code (`policy_out`,
// `policy_in`, `not_allowed`, `not_member`, ...), `message` the exact text
// the UI shows verbatim (docs/FAMILIES_TASKS.md 3.5). `request()` below
// already unwraps a `{detail: {...}}` envelope into `detail`, so by the
// time it reaches here a `{reason, message}` body looks the same whether
// the relay nested it under `detail` or sent it bare.
function parseReasonMessage(detail: unknown): { reason?: string; message?: string } | null {
  if (!detail || typeof detail !== "object") return null;
  const d = detail as Record<string, unknown>;
  if (typeof d.reason !== "string" && typeof d.message !== "string") return null;
  return {
    reason: typeof d.reason === "string" ? d.reason : undefined,
    message: typeof d.message === "string" ? d.message : undefined,
  };
}

export class ApiError extends Error {
  status: number;
  detail: unknown;
  /** Machine-readable reason code from a `{reason, message}` 403 body,
   * when the relay sent one -- `undefined` for every other error shape. */
  reason?: string;

  constructor(status: number, detail: unknown) {
    const parsed = parseReasonMessage(detail);
    super(parsed?.message ?? (typeof detail === "string" ? detail : `request failed with status ${status}`));
    this.status = status;
    this.detail = detail;
    this.reason = parsed?.reason;
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

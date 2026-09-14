import type { Timestamp } from "firebase/firestore";

import { LOC_REQ_TTL_MS, type MessageDoc } from "./types";

/** Firestore `Timestamp | null` -> epoch ms, or `null` if unset (a doc read
 * optimistically right after a local write can have a pending server
 * timestamp, which `firebase/firestore` surfaces as `null` until it
 * resolves). */
export function tsToMillis(ts: Timestamp | null | undefined): number | null {
  if (!ts) return null;
  return ts.toMillis();
}

export function formatClock(ms: number | null): string {
  if (ms === null) return "";
  return new Date(ms).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
}

export function formatRelativeAge(ms: number): string {
  const deltaS = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (deltaS < 60) return `${deltaS}s ago`;
  const deltaM = Math.round(deltaS / 60);
  if (deltaM < 60) return `${deltaM}m ago`;
  const deltaH = Math.round(deltaM / 60);
  if (deltaH < 24) return `${deltaH}h ago`;
  const deltaD = Math.round(deltaH / 24);
  return `${deltaD}d ago`;
}

/** PROTOCOL.md §13.4 / SERVER_PLAN.md §5.6: a `loc_req` still `sent` (never
 * `fulfilled`) 15 minutes after `createdAt` is `expired` -- derived at read
 * time, not stored. Mirrors the same rule for a plain message's down-ack
 * (PROTOCOL.md §4), which this app does not otherwise need to derive since
 * the pager backend's only non-terminal states here are `queued`/`sent`. */
export function isLocReqExpired(msg: Pick<MessageDoc, "kind" | "createdAt">): boolean {
  if (msg.kind !== "loc_req") return false;
  const createdMs = tsToMillis(msg.createdAt);
  if (createdMs === null) return false;
  return Date.now() - createdMs > LOC_REQ_TTL_MS;
}

"use client";

/** Address book client -- docs/ADDRESS_BOOK_DESIGN.md decisions 3-5, 11.
 * Reads `GET /api/book`, writes a per-owner nickname with PUT/DELETE
 * `/api/book/{owner}/entries/{peer}`. */

import { useCallback, useEffect, useState } from "react";

import { api } from "./api";

export type BookKind = "person" | "external" | "group";

export interface BookEntry {
  uid: string | null;
  alias: string;
  kind: BookKind;
  displayName: string;
  nick: string | null;
  label: string;
  phone?: string | null;
  // docs/BRIDGE_PHONE_DESIGN.md decision 7: set on a bridged Google Chat /
  // Google Voice contact (`t:"chat"` on the pager).
  chat?: { source: "gchat" | "gvoice" } | null;
  inFamily: boolean;
  sendable: boolean;
  reason?: string | null;
  onPager: boolean;
}

export interface BookResponse {
  ownerUid: string;
  bv: number | null;
  pagerCap: number;
  truncated: boolean;
  entries: BookEntry[];
}

export const NICK_MAX_CODEPOINTS = 16;
export const NICK_MAX_BYTES = 48;

export type BookGroup = "Family" | "People" | "Numbers" | "Groups";
export const BOOK_GROUP_ORDER: BookGroup[] = ["Family", "People", "Numbers", "Groups"];

/** Family = person in the owner's family, People = other person, Numbers =
 * external, Groups = group. */
export function bookGroup(e: BookEntry): BookGroup {
  if (e.kind === "group") return "Groups";
  if (e.kind === "external") return "Numbers";
  return e.inFamily ? "Family" : "People";
}

/** Client-side mirror of the relay's nickname bounds (the wire's `n`).
 * `""` is valid and means "clear". */
export function nickError(v: string): string | null {
  const t = v.trim();
  if (!t) return null;
  if ([...t].length > NICK_MAX_CODEPOINTS) return `At most ${NICK_MAX_CODEPOINTS} characters`;
  if (new TextEncoder().encode(t).length > NICK_MAX_BYTES) {
    return `Too long once encoded (at most ${NICK_MAX_BYTES} bytes)`;
  }
  if (/\p{C}/u.test(t)) return "Control characters are not allowed";
  return null;
}

interface BookState {
  key: string;
  data: BookResponse | null;
  error: string | null;
}

export function useBook(uid?: string): {
  data: BookResponse | null;
  error: string | null;
  loading: boolean;
  reload: () => void;
} {
  const [nonce, setNonce] = useState(0);
  const [state, setState] = useState<BookState | null>(null);
  const key = `${uid ?? ""}#${nonce}`;

  useEffect(() => {
    let cancelled = false;
    api
      .get<BookResponse>(`/book${uid ? `?uid=${encodeURIComponent(uid)}` : ""}`)
      .then((data) => {
        if (!cancelled) setState({ key, data, error: null });
      })
      .catch((e: unknown) => {
        if (!cancelled) {
          setState({ key, data: null, error: e instanceof Error ? e.message : "Could not load the address book" });
        }
      });
    return () => {
      cancelled = true;
    };
  }, [uid, key]);

  const reload = useCallback(() => setNonce((n) => n + 1), []);
  const current = state && state.key === key ? state : null;
  return {
    // Keep showing the previous result while a reload is in flight.
    data: current ? current.data : (state?.data ?? null),
    error: current ? current.error : null,
    loading: current === null,
    reload,
  };
}

/** `""` clears the nickname (DELETE), anything else sets it (PUT). */
export async function saveNick(owner: string, peer: string, nick: string): Promise<void> {
  const path = `/book/${encodeURIComponent(owner)}/entries/${encodeURIComponent(peer)}`;
  const t = nick.trim();
  if (!t) {
    await api.del(path);
  } else {
    await api.put(path, { nick: t });
  }
}

export function reasonText(reason?: string | null): string {
  switch (reason) {
    case "policy_out":
      return "Blocked by this person's outgoing policy";
    case "policy_in":
      return "Blocked by their inbound policy";
    case "sms_contact":
      return "SMS contacts can only be texted from a pager";
    case "no_sms_number":
      return "needs an SMS number";
    case "no_bridge":
      return "Bridge phone not set up";
    case "not_allowed":
      return "Not approved";
    default:
      return "Can't message";
  }
}

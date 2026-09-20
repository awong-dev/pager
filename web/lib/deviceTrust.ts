/**
 * Pure helpers for the CA-trust chip on `/admin/devices` --
 * docs/V02_DESIGN.md §4.3/§4.4. Kept free of React/MUI so it is trivially
 * testable (see the module docstring in `lib/types.ts` for where `tls`/
 * `caFp` come from -- `devices/{id}.status`, both absent on older firmware).
 */

import type { TlsState } from "./types";

export type TrustChipVariant = "pinned" | "broken" | "unpinned";

export interface TrustChipInfo {
  variant: TrustChipVariant;
  label: string;
  tooltip: string;
  color: "success" | "warning" | "default";
}

const BROKEN_TOOLTIP =
  "The pager could not verify the broker's certificate and is running without verification so that pages still arrive. " +
  "This does not by itself mean an attack. Pushing the current CA usually fixes it.";

/** `null` means "render nothing" -- an absent `tls` field (older firmware). */
export function trustChipInfo(
  tls: TlsState | null | undefined,
  caFp: string | null | undefined
): TrustChipInfo | null {
  if (tls === "pinned") {
    return {
      variant: "pinned",
      label: "Server verified",
      tooltip: caFp ? `Pinned CA fingerprint: ${caFp}` : "Pinned CA fingerprint not yet reported.",
      color: "success",
    };
  }
  if (tls === "broken") {
    return {
      variant: "broken",
      label: "Server not verified",
      tooltip: BROKEN_TOOLTIP,
      color: "warning",
    };
  }
  if (tls === "unpinned") {
    return {
      variant: "unpinned",
      label: "No CA pinned",
      tooltip: "This pager has never had a broker certificate pinned.",
      color: "default",
    };
  }
  return null;
}

/** `docs/V02_DESIGN.md` §5: `locBackoffS` is seconds until the next GPS
 * attempt is allowed, 0/absent = now. Renders `null` when there is nothing
 * worth telling a parent. */
export function locBackoffLabel(locBackoffS: number | null | undefined): string | null {
  if (!locBackoffS || locBackoffS <= 0) return null;
  const minutes = Math.max(1, Math.round(locBackoffS / 60));
  return `next GPS attempt allowed in ~${minutes} min`;
}

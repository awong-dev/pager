/** Pure helpers for the OTA firmware UI -- docs/OTA_DESIGN.md §4, §5. */

import { useEffect, useState } from "react";

import { api } from "@/lib/api";
import { familyQuery } from "@/lib/family-context";
import type { DeviceStatusDoc } from "@/lib/types";

/** The 10 MB monthly cellular target the design budgets against (§4). */
export const MONTHLY_TARGET_BYTES = 10_000_000;

export interface FirmwareBuild {
  id16: string;
  version: string;
  size: number;
  published: number; // epoch seconds
  kind: "full" | "delta";
  osz: number;
  estBytes: number;
  /** True: preview says "full" but the push will generate a delta. */
  onDemandDelta?: boolean;
}

export interface OtaPushResult {
  ok: boolean;
  kind?: "full" | "delta";
  osz?: number;
  estBytes?: number;
  onDemand?: boolean;
}

type OtaStatus = Pick<DeviceStatusDoc, "otaState" | "otaPct" | "otaErr">;

/** One-line job state for the Firmware chip; null when no job reported. */
export function otaStateLabel(status?: OtaStatus | null): string | null {
  switch (status?.otaState) {
    case "wait":
      return "Update queued";
    case "dl":
      return `Downloading ${status.otaPct ?? 0} %`;
    case "ready":
      return "Ready, installs when idle";
    case "inst":
      return "Installing…";
    case "ok":
      return "Updated";
    case "fail":
      return `Update failed (${status.otaErr ?? "unknown"})`;
    case "rb":
      return `Rolled back (${status.otaErr ?? "boot"})`;
    default:
      return null;
  }
}

/** 1000-based, matching the design doc: "339 KB", "1.1 MB". */
export function formatBytes(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)} MB`;
  if (n >= 1000) return `${Math.round(n / 1000)} KB`;
  return `${n} B`;
}

/** Which relay route family the OTA calls go to: `admin` is the super-only
 *  `/api/admin/*`, `family` the family-admin `/api/family/*` (scoped by
 *  `familyQuery()`; the relay requires `device=` on the build list). */
export type FirmwareScope = "admin" | "family";

/** `familyQuery()` is `?family=x` or ""; join it onto a path that may already
 *  carry a query string. */
function withFamily(path: string): string {
  const fq = familyQuery();
  if (!fq) return path;
  return path + (path.includes("?") ? "&" : "?") + fq.slice(1);
}

export async function listBuilds(scope: FirmwareScope, deviceId?: string): Promise<FirmwareBuild[]> {
  let path: string;
  if (scope === "family") {
    if (!deviceId) throw new Error("device id required for the family build list");
    path = withFamily(`/family/firmware?device=${encodeURIComponent(deviceId)}`);
  } else {
    path = deviceId ? `/admin/firmware?device=${encodeURIComponent(deviceId)}` : "/admin/firmware";
  }
  const r = await api.get<{ builds: FirmwareBuild[] }>(path);
  return r.builds;
}

/** Newest published build (the API lists newest first); fetched when
 *  `enabled`. The family scope needs `deviceId` (the relay requires it) and
 *  stays idle without one. Undefined until loaded or on error. */
export function useNewestBuild(
  enabled: boolean,
  scope: FirmwareScope,
  deviceId?: string
): FirmwareBuild | undefined {
  const [newest, setNewest] = useState<FirmwareBuild | undefined>(undefined);
  const ready = enabled && (scope === "admin" || !!deviceId);
  useEffect(() => {
    if (!ready) return;
    let cancelled = false;
    listBuilds(scope, deviceId)
      .then((b) => {
        if (!cancelled) setNewest(b[0]);
      })
      .catch(() => {
        // Chip falls back to the job-state label without a newest build.
      });
    return () => {
      cancelled = true;
    };
  }, [ready, scope, deviceId]);
  return newest;
}

function otaPath(scope: FirmwareScope, deviceId: string): string {
  const p = `/${scope}/devices/${encodeURIComponent(deviceId)}/ota`;
  return scope === "family" ? withFamily(p) : p;
}

export function pushOta(
  scope: FirmwareScope,
  deviceId: string,
  target16: string
): Promise<OtaPushResult> {
  return api.post<OtaPushResult>(otaPath(scope, deviceId), { target: target16 });
}

export function cancelOta(scope: FirmwareScope, deviceId: string): Promise<OtaPushResult> {
  return api.post<OtaPushResult>(otaPath(scope, deviceId), { cancel: true });
}

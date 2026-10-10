/** Pure helpers for the OTA firmware UI -- docs/OTA_DESIGN.md §4, §5. */

import { useEffect, useState } from "react";

import { api } from "@/lib/api";
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

export async function listBuilds(deviceId?: string): Promise<FirmwareBuild[]> {
  const r = await api.get<{ builds: FirmwareBuild[] }>(
    deviceId ? `/admin/firmware?device=${encodeURIComponent(deviceId)}` : "/admin/firmware"
  );
  return r.builds;
}

/** Newest published build (the API lists newest first); fetched once when
 *  `enabled` (the endpoint is super-only). Undefined until loaded or on error. */
export function useNewestBuild(enabled: boolean): FirmwareBuild | undefined {
  const [newest, setNewest] = useState<FirmwareBuild | undefined>(undefined);
  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;
    listBuilds()
      .then((b) => {
        if (!cancelled) setNewest(b[0]);
      })
      .catch(() => {
        // Chip falls back to the job-state label without a newest build.
      });
    return () => {
      cancelled = true;
    };
  }, [enabled]);
  return newest;
}

export function pushOta(deviceId: string, target16: string): Promise<OtaPushResult> {
  return api.post<OtaPushResult>(`/admin/devices/${encodeURIComponent(deviceId)}/ota`, {
    target: target16,
  });
}

export function cancelOta(deviceId: string): Promise<OtaPushResult> {
  return api.post<OtaPushResult>(`/admin/devices/${encodeURIComponent(deviceId)}/ota`, {
    cancel: true,
  });
}

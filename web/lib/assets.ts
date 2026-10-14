/** Helpers for the font (assets) OTA UI -- docs/ASSETS_OTA_DESIGN.md §3. */

import { useEffect, useState } from "react";

import { api } from "@/lib/api";
import { familyQuery } from "@/lib/family-context";
import type { FirmwareScope } from "@/lib/firmware";
import type { DeviceDoc } from "@/lib/types";

/** One published font image as listed by `GET /api/{family,admin}/assets`.
 *  The relay's `AsBuild` carries `id` (64 hex), `label`, `lang`, `fv`, `size`,
 *  `published` and `full.{osz,osha}`; `id16`, `osz` and `estBytes` are
 *  accepted too if the route precomputes them. */
export interface AssetBuild {
  id: string;
  id16: string;
  label: string;
  lang: string;
  size: number;
  published: number; // epoch seconds
  osz: number;
  estBytes: number;
}

interface RawAssetBuild {
  id?: string;
  id16?: string;
  label?: string;
  lang?: string;
  size?: number;
  published?: number;
  osz?: number;
  estBytes?: number;
  full?: { osz?: number };
}

/** Same estimate as the design (§4): zlib size * 1.045 + 7 KB of overhead. */
export function estAssetBytes(osz: number): number {
  return Math.round(osz * 1.045 + 7168);
}

function normalize(b: RawAssetBuild): AssetBuild {
  const id = (b.id ?? b.id16 ?? "").toLowerCase();
  const osz = b.osz ?? b.full?.osz ?? 0;
  return {
    id,
    id16: (b.id16 ?? id).slice(0, 16).toLowerCase(),
    label: b.label ?? "",
    lang: b.lang ?? "",
    size: b.size ?? 0,
    published: b.published ?? 0,
    osz,
    estBytes: b.estBytes ?? estAssetBytes(osz),
  };
}

function withFamily(path: string): string {
  const fq = familyQuery();
  if (!fq) return path;
  return path + (path.includes("?") ? "&" : "?") + fq.slice(1);
}

export async function listAssets(scope: FirmwareScope, deviceId: string): Promise<AssetBuild[]> {
  const p = `/${scope}/assets?device=${encodeURIComponent(deviceId)}`;
  const r = await api.get<{ builds: RawAssetBuild[] }>(scope === "family" ? withFamily(p) : p);
  return r.builds.map(normalize);
}

function assetsPath(scope: FirmwareScope, deviceId: string): string {
  const p = `/${scope}/devices/${encodeURIComponent(deviceId)}/assets`;
  return scope === "family" ? withFamily(p) : p;
}

export function pushAssets(scope: FirmwareScope, deviceId: string, target16: string): Promise<{ ok: boolean }> {
  return api.post<{ ok: boolean }>(assetsPath(scope, deviceId), { target: target16 });
}

export function cancelAssets(scope: FirmwareScope, deviceId: string): Promise<{ ok: boolean }> {
  return api.post<{ ok: boolean }>(assetsPath(scope, deviceId), { cancel: true });
}

/** True while the device's current job (whichever kind) is a font job. */
export function isAssetJob(d?: Pick<DeviceDoc, "otaJob"> | null): boolean {
  return d?.otaJob?.part === "assets";
}

/** Builds list for the device card label lookup; fetched only when `enabled`. */
export function useAssetBuilds(enabled: boolean, scope: FirmwareScope, deviceId: string): AssetBuild[] {
  const [builds, setBuilds] = useState<AssetBuild[]>([]);
  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;
    listAssets(scope, deviceId)
      .then((b) => {
        if (!cancelled) setBuilds(b);
      })
      .catch(() => {
        // The line falls back to the bare id16.
      });
    return () => {
      cancelled = true;
    };
  }, [enabled, scope, deviceId]);
  return builds;
}

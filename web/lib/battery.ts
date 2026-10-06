/** Client for `GET /api/devices/{id}/battery` (docs/BATTERY_STATS_DESIGN.md
 * B6, TASK_batt_backend step 6). The relay caps one request at 90 days and
 * 5000 samples; this splits a longer span into 30-day requests. */

import { api } from "@/lib/api";
import type { BatteryModel, BatterySample } from "@/lib/batteryModel";

export interface BatteryResponse {
  deviceId: string;
  since: number;
  until: number;
  samples: BatterySample[];
  truncated: boolean;
  model: BatteryModel;
}

export interface BatteryResult {
  samples: BatterySample[];
  truncated: boolean;
  model: BatteryModel | null;
}

const CHUNK_S = 30 * 86400;

export async function fetchBattery(deviceId: string, since: number, until: number): Promise<BatteryResult> {
  const byId = new Map<string, BatterySample>();
  const noId: BatterySample[] = [];
  let truncated = false;
  let model: BatteryModel | null = null;
  for (let a = since; a <= until; a += CHUNK_S) {
    const b = Math.min(a + CHUNK_S, until);
    const r = await api.get<BatteryResponse>(`/devices/${deviceId}/battery?since=${a}&until=${b}`);
    for (const s of r.samples) {
      if (s.id) byId.set(s.id, s);
      else noId.push(s);
    }
    truncated = truncated || r.truncated;
    model = r.model;
    if (b >= until) break;
  }
  const samples = [...byId.values(), ...noId].sort((x, y) => x.ts - y.ts);
  return { samples, truncated, model };
}

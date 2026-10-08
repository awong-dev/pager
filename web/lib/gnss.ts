/**
 * GNSS enable flag -- docs/GNSS_DISABLE_DESIGN.md D5.
 *
 *   GET /api/devices/{id}/gnss -> GnssConfigResponse
 *   PUT /api/devices/{id}/gnss body GnssPutRequest -> GnssConfigResponse
 */

export interface GnssConfigResponse {
  en: boolean;
  pending: boolean; // cfg.loc pushed, not yet acked `shown`
  reported: number | null; // last /status `gnss` (0/1); null = not reported
}

export interface GnssPutRequest {
  en: boolean;
}

export function reportedLabel(reported: number | null): string {
  if (reported === 1) return "on";
  if (reported === 0) return "off";
  return "unknown";
}

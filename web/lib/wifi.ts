/**
 * WiFi provisioning panel -- docs/WIFI_DESIGN.md §4, docs/WIFI_TASKS.md W8.
 * Types for the relay contract this task was built against (W7, the relay
 * side, was being built in parallel on this branch -- treat the exact
 * response shapes below as the contract to reconcile against, not a
 * confirmed observation; see `components/WifiPanel.tsx`'s module docstring
 * for the full assumed request/response shape) and pure validation helpers
 * kept free of React/MUI so they can be unit tested without a DOM, mirroring
 * `lib/smsContacts.ts`'s split.
 *
 *   GET /api/devices/{id}/wifi -> WifiConfigResponse
 *   PUT /api/devices/{id}/wifi body WifiPutRequest -> WifiConfigResponse
 *     422 on bad input; 409 (guard: last `/status.tls` isn't `pinned`) when
 *     the PUT carries `nets` -- `en` alone is always allowed.
 */

// A GET never carries a PSK (docs/WIFI_DESIGN.md §4: "GET never returns a
// stored PSK; it returns {s, set: true}"). A PUT carries a PSK only for a
// network the caller is (re)setting -- `set` is meaningless there and never
// sent.
export interface WifiNetwork {
  s: string; // SSID
  p?: string; // PSK -- PUT only, write-only, never present on a GET response
  set?: boolean; // GET only: true if a PSK is stored for this slot
}

export interface WifiConfigResponse {
  en: boolean;
  nets: WifiNetwork[]; // 0-2 entries, in NVS slot order (s0/p0 first)
  // ASSUMED, matching `SmsContactsResponse.pending` -- docs/WIFI_DESIGN.md
  // §4 says the `cfg.wifi` push is acked `shown` "identical to cfg.sms's
  // contract", which is exactly what `pending` reflects on the sms-contacts
  // route. Not literally spelled out for this route in W7's text; treat as
  // optional so the UI degrades if the relay omits it.
  pending?: boolean;
}

// `nets` omitted entirely (not `[]`) means "leave the stored networks
// alone, apply `en` only" -- docs/WIFI_DESIGN.md §4. `nets: []` clears them.
export interface WifiPutRequest {
  en: boolean;
  nets?: WifiNetwork[];
}

export const WIFI_NET_MAX = 2;
// docs/WIFI_DESIGN.md §4: "SSID <=32 bytes, PSK 8-63 bytes" (WPA2-PSK only).
export const WIFI_SSID_MAX_BYTES = 32;
export const WIFI_PSK_MIN_BYTES = 8;
export const WIFI_PSK_MAX_BYTES = 63;

const textEncoder = new TextEncoder();

export function isValidSsid(ssid: string): boolean {
  const bytes = textEncoder.encode(ssid);
  return bytes.length >= 1 && bytes.length <= WIFI_SSID_MAX_BYTES;
}

export function isValidPsk(psk: string): boolean {
  const bytes = textEncoder.encode(psk);
  return bytes.length >= WIFI_PSK_MIN_BYTES && bytes.length <= WIFI_PSK_MAX_BYTES;
}

/** One editable row in the panel: local UI state, not the wire shape.
 * `hadStoredPsk` mirrors the GET response's `set: true` for this slot so the
 * password field can show a `••••••` placeholder
 * without ever holding a real PSK value from the server. */
export interface WifiRow {
  ssid: string;
  psk: string; // always "" unless the user is typing/changing it
  hadStoredPsk: boolean;
}

export function emptyRow(): WifiRow {
  return { ssid: "", psk: "", hadStoredPsk: false };
}

/** Validates the rows that will be sent as `nets` on Save. Every included
 * row (non-empty SSID) must carry a freshly-typed PSK: the server never
 * hands a stored PSK back (docs/WIFI_DESIGN.md §4), and `cfg.wifi`'s `nets`
 * is a wholesale replace (`WIFI_TASKS.md` W3: "whole push refused, not
 * truncated" on any per-entry problem), so there is no way to resend an
 * unmodified row's old password. This is a real UX consequence of the
 * "PSK never displayed back" rule, not an invented API restriction -- see
 * the module docstring in `components/WifiPanel.tsx`. */
export function validateRows(rows: WifiRow[]): string[] {
  const errors: string[] = [];
  if (rows.length > WIFI_NET_MAX) {
    errors.push(`No more than ${WIFI_NET_MAX} networks are allowed.`);
  }
  const seenSsids = new Set<string>();
  rows.forEach((r, i) => {
    const row = i + 1;
    const ssid = r.ssid.trim();
    if (ssid.length === 0) return; // an empty row is just not sent
    if (!isValidSsid(ssid)) {
      errors.push(`Network ${row}: SSID must be 1-${WIFI_SSID_MAX_BYTES} bytes.`);
    } else if (seenSsids.has(ssid)) {
      errors.push(`Network ${row}: duplicate SSID.`);
    }
    seenSsids.add(ssid);
    if (!isValidPsk(r.psk)) {
      errors.push(
        r.hadStoredPsk
          ? `Network ${row}: re-enter this network's password to keep it saved (it isn't stored in the browser).`
          : `Network ${row}: password must be ${WIFI_PSK_MIN_BYTES}-${WIFI_PSK_MAX_BYTES} bytes.`
      );
    }
  });
  return errors;
}

/** Builds the `nets` array to PUT from the current rows -- only non-empty
 * rows, trimmed. `undefined` when there is nothing to send (every row is
 * blank), which callers use to decide whether to omit `nets` entirely from
 * the request (leave stored networks untouched) vs. send `nets: []`
 * (explicit Clear). */
export function rowsToNets(rows: WifiRow[]): WifiNetwork[] {
  return rows
    .filter((r) => r.ssid.trim().length > 0)
    .map((r) => ({ s: r.ssid.trim(), p: r.psk }));
}

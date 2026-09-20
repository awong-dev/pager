/**
 * Device-direct SMS -- docs/V02_DESIGN.md §6. Types for the relay contract
 * this task was built against (relay work was in progress in parallel; see
 * `web/app/devices/[id]/DevicePageClient.tsx`'s module docstring for the
 * exact routes) and pure validation/formatting helpers kept free of
 * React/MUI so they can be unit tested without a DOM.
 *
 *   GET  /api/devices/{id}/sms-contacts -> SmsContactsResponse
 *   PUT  /api/devices/{id}/sms-contacts body {"contacts": SmsContact[]} -> SmsContactsResponse
 *   GET  /api/devices/{id}/sms-log?limit=&before= -> SmsLogResponse (newest first)
 *   GET  /api/devices -> DeviceListItem[] (the caller's own devices)
 */

export interface SmsContact {
  name: string;
  phone: string;
}

export interface SmsContactsResponse {
  contacts: SmsContact[];
  pending: boolean;
}

export type SmsDirection = "out" | "in";
export type SmsStatus = "sent" | "failed" | "recv" | "blocked";

export interface SmsLogEntry {
  id: string;
  ts: number;
  smsTs: number;
  dir: SmsDirection;
  peer: string;
  name: string | null;
  st: SmsStatus;
  body: string;
}

export interface SmsLogResponse {
  entries: SmsLogEntry[];
}

// `GET /api/devices` -- not yet part of `lib/types.ts`'s Firestore-doc
// mirrors (this is an API response, not a document this app reads
// directly); mirrors the same `status` shape as `DeviceStatusDoc` on a
// best-effort basis since it is being built by another agent in parallel.
export interface DeviceListItem {
  id: string;
  label: string;
  status?: {
    state?: string | null;
    tls?: "unpinned" | "pinned" | "broken" | null;
    caFp?: string | null;
    smsLost?: number | null;
  } | null;
}

export const SMS_CONTACT_MAX = 8;
export const SMS_NAME_MAX = 16;
// docs/V02_DESIGN.md §6 says "name <= 16 chars", but `relay/app/store/
// devices.py`'s `SmsContact` additionally caps the name at 24 UTF-8 bytes
// (`SMS_CONTACT_NAME_MAX_UTF8_BYTES`, a documented deviation there to keep
// 8 maximal contacts inside the signed `cfg.sms` envelope's byte budget) --
// mirrored here so a name that is 16 *codepoints* but more than 24 UTF-8
// bytes (e.g. wide non-Latin scripts) is caught before the round trip
// instead of only surfacing as the relay's 422 on Save.
export const SMS_NAME_MAX_UTF8_BYTES = 24;
// docs/V02_DESIGN.md §6: "Max 8 entries {name <= 16 chars, phone E.164}".
export const PHONE_E164_RE = /^\+[1-9]\d{6,14}$/;
export const PHONE_E164_EXAMPLE = "+12065550100";

const textEncoder = new TextEncoder();

export function isValidName(name: string): boolean {
  const trimmed = name.trim();
  if (trimmed.length < 1 || trimmed.length > SMS_NAME_MAX) return false;
  return textEncoder.encode(trimmed).length <= SMS_NAME_MAX_UTF8_BYTES;
}

export function isValidPhone(phone: string): boolean {
  return PHONE_E164_RE.test(phone.trim());
}

/** Validates the whole contact list client-side, mirroring the relay's PUT
 * validation (docs/V02_DESIGN.md §6: max 8, name 1-16 chars, E.164 phone,
 * no duplicate phones) so a bad list is caught before the round trip.
 * Returns one human-readable problem per row/list issue; empty = valid. */
export function validateContacts(contacts: SmsContact[]): string[] {
  const errors: string[] = [];
  if (contacts.length > SMS_CONTACT_MAX) {
    errors.push(`No more than ${SMS_CONTACT_MAX} contacts are allowed.`);
  }
  const seenPhones = new Set<string>();
  contacts.forEach((c, i) => {
    const row = i + 1;
    if (!isValidName(c.name)) {
      errors.push(`Row ${row}: name must be 1-${SMS_NAME_MAX} characters.`);
    }
    if (!isValidPhone(c.phone)) {
      errors.push(`Row ${row}: phone must be E.164, e.g. ${PHONE_E164_EXAMPLE}.`);
    } else {
      const normalized = c.phone.trim();
      if (seenPhones.has(normalized)) {
        errors.push(`Row ${row}: duplicate phone number.`);
      }
      seenPhones.add(normalized);
    }
  });
  return errors;
}

/** Direction arrow for the SMS log table -- pure so row rendering is
 * testable without a DOM. */
export function dirArrow(dir: SmsDirection): string {
  return dir === "out" ? "→" : "←";
}

export function dirLabel(dir: SmsDirection): string {
  return dir === "out" ? "sent to pager's contact" : "received by pager";
}

export type StatusColor = "success" | "error" | "warning" | "default";

export function statusColor(st: SmsStatus): StatusColor {
  if (st === "sent" || st === "recv") return "success";
  if (st === "failed") return "error";
  return "warning"; // blocked
}

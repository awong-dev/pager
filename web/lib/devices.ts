/** Device naming and label-edit helpers. The id is a machine-issued key
 *  (`pgr-` + 8 hex); the label is the human-facing, editable name. */

import { api } from "@/lib/api";
import { familyQuery } from "@/lib/family-context";
import type { FirmwareScope } from "@/lib/firmware";

/** Max label length in characters, client side. The relay counts UTF-8 bytes
 *  (1..32) and answers 422 otherwise; that message is shown as-is. */
export const DEVICE_LABEL_MAX_CHARS = 32;

/** The device's display name: its label, or the id when the label is empty. */
export function deviceName(d: { id?: string; label?: string | null }): string {
  return d.label?.trim() ? d.label : (d.id ?? "");
}

/** `PATCH /api/{admin|family}/devices/{id}` `{label}`; the Firestore listener
 *  refreshes the row. The pager shows it after its next credential rotation. */
export function patchDeviceLabel(scope: FirmwareScope, deviceId: string, label: string): Promise<unknown> {
  const p = `/${scope}/devices/${encodeURIComponent(deviceId)}`;
  return api.patch(scope === "family" ? p + familyQuery() : p, { label });
}

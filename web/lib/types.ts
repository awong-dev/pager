/**
 * TypeScript mirrors of the Firestore document shapes the web app reads
 * directly (docs/SERVER_PLAN.md §3, §5.1: "Reads that the web app can do
 * straight from Firestore ... have no API endpoint"). Field names match
 * `relay/app/store/*.py`'s pydantic models exactly -- this file has no
 * relay-side counterpart to import from (Python/TypeScript split), so it is
 * kept deliberately narrow (only the fields the UI actually renders) and
 * commented with the store module it mirrors so the two can be diffed by
 * eye when the schema changes.
 */

import type { Timestamp } from "firebase/firestore";

// ---- users/{uid} -- app/store/users.py ----
export type Role = "admin" | "member";

export interface UserDoc {
  alias: string;
  displayName: string;
  email: string | null;
  phone: string | null;
  role: Role;
  disabled: boolean;
  createdAt: Timestamp | null;
}

// ---- users/{uid}/backends/{bid} -- app/store/backends.py ----
export type BackendKind = "pager" | "webapp" | "sms" | "gchat";

export interface BackendDoc {
  kind: BackendKind;
  config: Record<string, unknown>;
  enabled: boolean;
  verifiedAt: Timestamp | null;
}

// ---- allow/{fromUid}_{toUid} -- app/store/allow.py ----
export interface AllowEdgeDoc {
  fromUid: string;
  toUid: string;
  message: boolean;
  locate: boolean;
}

// ---- devices/{deviceId} -- app/store/devices.py ----
export interface DeviceStatusDoc {
  state: string | null;
  mode: string | null;
  battMv: number | null;
  rssi: number | null;
  session: string | null;
  ts: number | null;
  fw: string | null;
  locPeriodS: number | null;
  locMinS: number | null;
  updatedAt: Timestamp | null;
}

export interface DeviceDoc {
  ownerUid: string;
  label: string;
  mqttUsername: string;
  defaultToUid: string | null;
  revokedAt: Timestamp | null;
  locatableBy: string[];
  status: DeviceStatusDoc;
}

// ---- devices/{deviceId}/locations/{autoId} -- app/store/locations.py ----
export interface LocationFixDoc {
  ts: number;
  fixTs: number;
  lat: number;
  lon: number;
  accM: number | null;
  src: string;
  cached: boolean;
  reqId: string | null;
  createdAt: Timestamp | null;
}

// ---- messages/{id} -- app/store/messages.py ----
export type MessageKind = "text" | "loc_req" | "loc";
export type DeliveryState =
  | "queued"
  | "sent"
  | "shown"
  | "read"
  | "fulfilled"
  | "failed"
  | "expired";

export interface DeliveryDoc {
  kind: string;
  state: DeliveryState;
  attempts: number;
  externalId: string | null;
  error: string | null;
  sentTs: number | null;
  shownTs: number | null;
  readTs: number | null;
}

export interface MessageLocDoc {
  lat: number;
  lon: number;
  accM?: number | null;
  fixTs: number;
  src?: string;
}

export interface MessageDoc {
  seq: number;
  convKey: string;
  uids: [string, string];
  senderUid: string;
  recipientUid: string;
  kind: MessageKind;
  body: string | null;
  loc: MessageLocDoc | null;
  wireId: string | null;
  originBackendKind: string | null;
  originBackendId: string | null;
  ts: number;
  createdAt: Timestamp | null;
  deliveries: Record<string, DeliveryDoc>;
  pendingDeviceIds: string[];
}

// ---- conversations/{convKey} -- app/store/messages.py ----
export interface ConversationDoc {
  uids: [string, string];
  lastMessageAt: Timestamp | null;
  lastPreview: string;
  unread: Record<string, number>;
}

// ---- settings/retention -- app/store/settings.py ----
export type RetentionUnit = "days" | "weeks";

export interface RetentionSettingDoc {
  n: number;
  unit: RetentionUnit;
}

export interface RetentionSettingsDoc {
  messages: RetentionSettingDoc;
  locations: RetentionSettingDoc;
}

// PROTOCOL.md §13.4 / SERVER_PLAN.md §5.6: a loc_req is `expired` 15 minutes
// after creation if never `fulfilled` -- derived at read time, not stored.
export const LOC_REQ_TTL_MS = 15 * 60 * 1000;

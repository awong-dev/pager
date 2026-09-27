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
export type TlsState = "unpinned" | "pinned" | "broken";

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
  // docs/V02_DESIGN.md §4.3 (CA trust): trust state and the pinned CA's short
  // fingerprint. Both absent on older firmware -- render nothing, not
  // "undefined".
  tls?: TlsState | null;
  caFp?: string | null;
  // docs/V02_DESIGN.md §5 (location): seconds until the next GPS attempt is
  // allowed, 0 = now. Absent on older firmware.
  locBackoffS?: number | null;
  // docs/V02_DESIGN.md §6 (device SMS): audit-queue entries dropped on the
  // pager before they could be uploaded. Absent on older firmware.
  smsLost?: number | null;
  // docs/WIFI_DESIGN.md §6: which transport carried the most recent MQTT
  // session, display/diagnosis only. Absent on firmware built before W7/W8
  // (older firmware never sends `xport`) -- render nothing, not "undefined".
  xport?: "lte" | "wifi" | null;
  // docs/LOCATION_TRACKING_DESIGN.md §5 R7 -- `app/store/devices.py`'s
  // `LastCell`: the serving cell from the most recent `/loc` report that
  // carried one, recorded whether or not that report's position was
  // resolvable (`ingest_loc` writes a `locations` doc only when it *was*
  // resolvable). Only `ts` is mirrored here -- it's the one field
  // `/location`'s timeline uses (a synthetic "cell only, position unknown"
  // row when this is newer than the newest stored fix); the raw
  // mcc/mnc/tac/ci/rsrp fields exist server-side but nothing in the web app
  // renders them today. Absent on a device that has never sent a `cell`.
  lastCell?: { ts: number } | null;
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
  // docs/LOCATION_TRACKING_DESIGN.md §5 R3/P3 -- relay/app/store/locations.py's
  // LocationFix. `why` is the reason this report was sent; `cellKey`
  // identifies the serving cell; `lastTs` is set only when this doc is a
  // dwell doc extended by a later report (null on a doc that has never been
  // extended).
  why?: string | null;
  cellKey?: string | null;
  lastTs?: number | null;
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
  // docs/GROUP_CHAT_DESIGN.md §2: one id shared by every copy of one logical
  // group message (the sender holds N-1 copies of their own message; the
  // client dedupes bubbles on this) and the author's alias, denormalised so
  // the client never needs a `users/{uid}` read to label a bubble. Both
  // null/absent on a DM or pager-originated message -- "not a group copy".
  groupMsgId: string | null;
  senderAlias: string | null;
}

// ---- conversations/{convKey} -- app/store/messages.py ----
export interface ConversationDoc {
  // docs/GROUP_CHAT_DESIGN.md §2: widened from a `[string, string]` DM pair
  // to the full member list for a group; a DM's `uids` is still exactly two
  // entries. `kind`/`name`/`alias`/`createdBy` are absent on every DM
  // document today -- absent `kind` MUST be read as `"dm"`.
  uids: string[];
  lastMessageAt: Timestamp | null;
  lastPreview: string;
  unread: Record<string, number>;
  kind?: "dm" | "group";
  name?: string;
  alias?: string;
  createdBy?: string;
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

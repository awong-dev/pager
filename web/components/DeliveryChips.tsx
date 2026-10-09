import Chip from "@mui/material/Chip";
import Stack from "@mui/material/Stack";

import { formatClock, isLocReqExpired } from "@/lib/time";
import type { DeliveryDoc, DeliveryState, MessageDoc } from "@/lib/types";

const STATE_COLOR: Record<DeliveryState, "default" | "info" | "success" | "warning" | "error"> = {
  queued: "default",
  sent: "info",
  shown: "info",
  read: "success",
  fulfilled: "success",
  failed: "error",
  expired: "warning",
};

function effectiveState(msg: MessageDoc, delivery: DeliveryDoc): DeliveryState {
  if (isLocReqExpired(msg) && (delivery.state === "queued" || delivery.state === "sent")) {
    return "expired";
  }
  return delivery.state;
}

function stateTimeMs(delivery: DeliveryDoc, state: DeliveryState): number | null {
  if (state === "read" && delivery.readTs) return delivery.readTs * 1000;
  if ((state === "shown" || state === "read") && delivery.shownTs) return delivery.shownTs * 1000;
  if (state === "sent" && delivery.sentTs) return delivery.sentTs * 1000;
  return null;
}

const WHERE: Record<string, string> = {
  pager: "pager",
  webapp: "app",
  gchat: "Google Chat",
  sms: "SMS",
  bridge: "Google Chat",
};

const LABELS: Record<string, Partial<Record<DeliveryState, string>>> = {
  pager: { queued: "waiting for pager", sent: "sent to pager", shown: "on pager", read: "read on pager" },
  webapp: { queued: "waiting", sent: "sent to app", shown: "shown in app", read: "read in app" },
  gchat: { queued: "waiting", sent: "sent to Google Chat", shown: "sent to Google Chat", read: "read in Google Chat" },
  // docs/BRIDGE_PHONE_DESIGN.md decision 7: a delivery handed to the bridge phone.
  bridge: {
    queued: "waiting for the phone",
    sent: "sent on Google Chat",
    shown: "sent on Google Chat",
    read: "sent on Google Chat",
  },
  sms: { queued: "waiting to send SMS", sent: "sent by SMS", shown: "sent by SMS", read: "sent by SMS" },
};

/** Chip text saying where a delivery went (docs/CONTACT_REQ_DESIGN.md
 * decision 4). Unknown kinds fall back to `kind: state`. */
export function chipLabel(kind: string, state: DeliveryState): string {
  const where = WHERE[kind];
  if (!where) return `${kind}: ${state}`;
  if (state === "failed") return `${where} failed`;
  if (state === "expired") return "expired";
  if (state === "fulfilled") return "location received";
  return LABELS[kind]?.[state] ?? `${kind}: ${state}`;
}

/** Per-backend delivery-state chips -- docs/SERVER_PLAN.md §7.4: "a parent
 * sees 'sent to pager 15:02 · sent by SMS'". Reads straight off the message
 * document's embedded `deliveries` map; no per-message listener needed. */
export default function DeliveryChips({ message }: { message: MessageDoc }) {
  const entries = Object.entries(message.deliveries);
  if (entries.length === 0) return null;

  return (
    <Stack direction="row" spacing={0.5} useFlexGap sx={{ mt: 0.5, flexWrap: "wrap" }}>
      {entries.map(([bid, delivery]) => {
        const state = effectiveState(message, delivery);
        const atMs = stateTimeMs(delivery, state);
        const label = `${chipLabel(delivery.kind, state)}${atMs ? " " + formatClock(atMs) : ""}`;
        return (
          <Chip
            key={bid}
            size="small"
            label={label}
            color={STATE_COLOR[state]}
            variant={state === "queued" ? "outlined" : "filled"}
          />
        );
      })}
    </Stack>
  );
}

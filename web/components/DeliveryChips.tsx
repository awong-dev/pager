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

/** Per-backend delivery-state chips -- docs/SERVER_PLAN.md §7.4: "a parent
 * sees 'pager: shown 15:02 · sms: sent'". Reads straight off the message
 * document's embedded `deliveries` map; no per-message listener needed. */
export default function DeliveryChips({ message }: { message: MessageDoc }) {
  const entries = Object.entries(message.deliveries);
  if (entries.length === 0) return null;

  return (
    <Stack direction="row" spacing={0.5} useFlexGap sx={{ mt: 0.5, flexWrap: "wrap" }}>
      {entries.map(([bid, delivery]) => {
        const state = effectiveState(message, delivery);
        const atMs = stateTimeMs(delivery, state);
        const label = `${delivery.kind}: ${state}${atMs ? " " + formatClock(atMs) : ""}`;
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

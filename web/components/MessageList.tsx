"use client";

/**
 * Shared read pane for `/chat/[alias]` (`ThreadPageClient.tsx`) and the
 * family-admin read-only `/chat/view/[key]` (`MonitorThreadClient.tsx`,
 * docs/FAMILIES_TASKS.md 2.4): the scrollable message list, its "Load
 * older" button and the "New messages" chip.
 *
 * All scroll bookkeeping -- the `listRef`, the auto-scroll layout effect,
 * mark-read, `pageSize` -- stays with the caller; this component only
 * renders what `messages` and the caller's scroll state say to, via plain
 * props and callbacks. That keeps `ThreadPageClient.tsx`'s existing
 * auto-scroll effect (docs/V03_PLAN.md §2) byte-for-byte unchanged by this
 * factoring: it still owns its own refs and still runs against its own
 * `listRef`, just handed to this component instead of a local JSX block.
 */

import Link from "next/link";
import type { RefObject, UIEvent } from "react";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";
import LocationOnIcon from "@mui/icons-material/LocationOn";

import DeliveryChips from "@/components/DeliveryChips";
import { formatClock, isLocReqExpired } from "@/lib/time";
import type { MessageDoc } from "@/lib/types";

export interface MessageRow extends MessageDoc {
  id: string;
}

// docs/SERVER_PLAN.md §7 (this task): the relay writes a `loc_req` message
// for every `/locate` call and a `kind='loc'` reply once it's answered
// (`app/location.py`) -- both render as one small link to `/location`
// instead of an inline request-state text/map card. `alias` is omitted by
// the read-only monitor thread (docs/FAMILIES_DESIGN.md §5.2: "no locate"),
// which renders the same row as plain, non-interactive text instead.
function LocRequestLinkRow({ alias, label }: { alias?: string; label: string }) {
  const content = (
    <Typography variant="caption" color="text.secondary">
      <LocationOnIcon fontSize="inherit" sx={{ verticalAlign: "middle" }} /> {label}
    </Typography>
  );
  const boxSx = {
    px: 1.5,
    py: 0.5,
    border: 1,
    borderColor: "divider",
    borderRadius: 2,
  } as const;
  return (
    <Stack direction="row" sx={{ justifyContent: "center", my: 1 }}>
      {alias ? (
        <Box
          component={Link}
          href={`/location?who=${encodeURIComponent(alias)}`}
          sx={{ ...boxSx, textDecoration: "none", color: "inherit" }}
        >
          {content}
        </Box>
      ) : (
        <Box sx={boxSx}>{content}</Box>
      )}
    </Stack>
  );
}

function LocReqRow({ message, mine, alias }: { message: MessageRow; mine: boolean; alias?: string }) {
  const delivery = Object.values(message.deliveries)[0];
  const expired = isLocReqExpired(message);
  const state =
    expired && delivery?.state !== "fulfilled" ? "expired" : (delivery?.state ?? "queued");
  return (
    <LocRequestLinkRow
      alias={alias}
      label={`${mine ? "You requested a location" : "Location requested"} -- ${state} -- view`}
    />
  );
}

function LocMessageRow({ message, mine, alias }: { message: MessageRow; mine: boolean; alias?: string }) {
  if (!message.loc) return null;
  return <LocRequestLinkRow alias={alias} label={`${mine ? "Location you shared" : "Location received"} -- view`} />;
}

function MessageBubble({
  message,
  mine,
  isGroup,
}: {
  message: MessageRow;
  mine: boolean;
  isGroup: boolean;
}) {
  const atMs = (message.ts ?? 0) * 1000;
  return (
    <Stack sx={{ alignItems: mine ? "flex-end" : "flex-start", my: 0.75 }}>
      {isGroup && message.senderAlias && (
        <Typography variant="caption" color="text.secondary" sx={{ px: 0.5 }}>
          {message.senderAlias}
        </Typography>
      )}
      <Box
        sx={{
          px: 1.5,
          py: 1,
          maxWidth: "80%",
          borderRadius: 2,
          bgcolor: mine ? "primary.main" : "grey.100",
          color: mine ? "primary.contrastText" : "text.primary",
        }}
      >
        <Typography variant="body1" sx={{ whiteSpace: "pre-wrap", wordBreak: "break-word" }}>
          {message.body}
        </Typography>
        <Typography variant="caption" sx={{ opacity: 0.7, display: "block", mt: 0.25 }}>
          {formatClock(atMs)}
        </Typography>
      </Box>
      {mine && <DeliveryChips message={message} />}
    </Stack>
  );
}

export interface MessageListProps {
  messages: MessageRow[];
  /** Uid whose messages render right-aligned ("mine"). `ThreadPageClient`
   * passes the signed-in account's own uid; the read-only monitor thread
   * has no signed-in party to the conversation, so it passes a fixed
   * "subject" uid instead (its own module docstring explains the choice). */
  meUid: string | null;
  isGroup: boolean;
  /** The route's own alias, for the loc_req/loc rows' "view" link to
   * `/location?who=`. Omit to render those rows as plain, non-interactive
   * text -- the monitor thread's "no locate" rule
   * (docs/FAMILIES_DESIGN.md §5.2). */
  locationAlias?: string;
  /** Whether to show the "Load older" button -- the caller decides this
   * (typically `messages.length >= pageSize`) since `pageSize` itself is
   * caller state. */
  canLoadOlder: boolean;
  onLoadOlder: () => void;
  listRef: RefObject<HTMLDivElement | null>;
  onScroll: (event: UIEvent<HTMLDivElement>) => void;
  showNewMessagesChip: boolean;
  onJumpToBottom: () => void;
}

export default function MessageList({
  messages,
  meUid,
  isGroup,
  locationAlias,
  canLoadOlder,
  onLoadOlder,
  listRef,
  onScroll,
  showNewMessagesChip,
  onJumpToBottom,
}: MessageListProps) {
  return (
    // `position: relative` lives on this wrapper, not the scrolling Box
    // below -- an absolutely positioned child of the scroll container
    // itself is positioned against that container's padding box at scroll
    // origin and scrolls away WITH the content, which is exactly when the
    // chip needs to stay visible. `minHeight: 0` keeps this flex child
    // shrinkable so the inner `overflowY: auto` box is the one that
    // actually scrolls, not this wrapper.
    <Box sx={{ position: "relative", flexGrow: 1, minHeight: 0, display: "flex", flexDirection: "column" }}>
      <Box ref={listRef} onScroll={onScroll} sx={{ flexGrow: 1, overflowY: "auto", px: 1 }}>
        {canLoadOlder && (
          <Stack direction="row" sx={{ justifyContent: "center", mb: 1 }}>
            <Button size="small" onClick={onLoadOlder}>
              Load older
            </Button>
          </Stack>
        )}
        {messages.map((m) => {
          const mine = m.senderUid === meUid;
          if (m.kind === "loc_req") {
            return <LocReqRow key={m.id} message={m} mine={mine} alias={locationAlias} />;
          }
          if (m.kind === "loc") {
            return <LocMessageRow key={m.id} message={m} mine={mine} alias={locationAlias} />;
          }
          return <MessageBubble key={m.id} message={m} mine={mine} isGroup={isGroup} />;
        })}
      </Box>
      {showNewMessagesChip && (
        <Chip
          label="New messages ↓"
          color="primary"
          onClick={onJumpToBottom}
          sx={{
            position: "absolute",
            bottom: 8,
            left: "50%",
            transform: "translateX(-50%)",
            cursor: "pointer",
            boxShadow: 2,
          }}
        />
      )}
    </Box>
  );
}

"use client";

/** `/chat/[alias]` -- docs/SERVER_PLAN.md §7.2/§7.4: the thread. One
 * `onSnapshot` query on `messages` (`convKey==k`, `orderBy(seq, desc)`,
 * `limit(50)`, "load older"); a 160-code-point composer; per-message
 * delivery chips read straight off the embedded `deliveries` map; a
 * "Request location" button gated on `allow.locate`; a last-known-location
 * card with an "open in maps" link.
 *
 * The `[alias]` segment is read from `usePathname()`, not Next's route
 * `params` -- this route is statically exported as a single placeholder
 * shell (`generateStaticParams` below; `output: 'export'` cannot pre-render
 * one HTML file per arbitrary future alias), and Firebase Hosting rewrites
 * every real `/chat/<alias>` request to that shell (web/firebase.json).
 * Reading the real alias from the browser's own URL is what makes the same
 * shell work for every alias, in dev and in the static export alike.
 *
 * A brand-new conversation (no message ever exchanged with this alias from
 * this browser) has no known peer uid yet -- see `lib/directory.tsx`'s
 * module docstring for why, and for the "send once, learn the uid from the
 * message we can read back" bootstrap this page performs.
 */

import {
  type Unsubscribe,
  collection,
  doc,
  getDoc,
  limit,
  onSnapshot,
  orderBy,
  query,
  where,
} from "firebase/firestore";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import Snackbar from "@mui/material/Snackbar";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import useMediaQuery from "@mui/material/useMediaQuery";
import LocationOnIcon from "@mui/icons-material/LocationOn";
import SendIcon from "@mui/icons-material/Send";

import AppShell from "@/components/AppShell";
import DeliveryChips from "@/components/DeliveryChips";
import LocationCard from "@/components/LocationCard";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { locBackoffLabel } from "@/lib/deviceTrust";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import { formatClock, isLocReqExpired } from "@/lib/time";
import type { AllowEdgeDoc, DeviceDoc, LocationFixDoc, MessageDoc } from "@/lib/types";

const BODY_MAX_CODEPOINTS = 160;
const BODY_MAX_UTF8_BYTES = 320;
const PAGE_SIZE_STEP = 50;
// How many of a device's most recent `locations` docs to keep around for
// the "last known location" card's faint trail (LocationMap) -- a plain
// Firestore listener limit, not a new endpoint.
const RECENT_FIXES_LIMIT = 8;
// docs/V03_PLAN.md §2: "within 80 px of the bottom" counts as at-bottom for
// both the auto-scroll and the mark-read gate.
const AT_BOTTOM_THRESHOLD_PX = 80;

interface MessageRow extends MessageDoc {
  id: string;
}

// `useLayoutEffect` warns when it runs during the `output: 'export'`
// prerender pass (no browser, so nothing to lay out before paint) --
// `ThreadInner` is in practice never reached there (`RequireAuth` renders
// only its loading screen until a real client-side auth state exists), but
// this keeps the hook honest instead of relying on that other file's guard.
const useIsomorphicLayoutEffect = typeof window !== "undefined" ? useLayoutEffect : useEffect;

/** First and last `seq` of a (seq-ascending) message array, or `null` for
 * an empty one -- the shape `ThreadInner`'s scroll effect diffs against the
 * previous render to tell "append" from "prepend" without relying on
 * array length (a prepend followed by a live append changes both ends at
 * once, and length alone can't tell them apart). */
interface SeqRange {
  first: number;
  last: number;
}

function seqRangeOf(messages: MessageRow[]): SeqRange | null {
  if (messages.length === 0) return null;
  return { first: messages[0]!.seq, last: messages[messages.length - 1]!.seq };
}

/** docs/GROUP_CHAT_DESIGN.md §2/§5: a group message's sender holds N-1
 * copies of it, one per recipient, all sharing one `groupMsgId` -- collapse
 * those into the single bubble a member should see. `groupMsgId` is absent
 * on a DM/pager message, so those pass through one row per document,
 * unchanged. Assumes `rows` is already `seq`-sorted; keeps the first (i.e.
 * lowest-`seq`) copy of each logical message, which is arbitrary but
 * deterministic -- every copy's `body`/`ts`/`senderAlias` are identical by
 * construction (§2), only `deliveries` differs per recipient. */
function dedupeByGroupMsgId(rows: MessageRow[]): MessageRow[] {
  const seen = new Set<string>();
  const result: MessageRow[] = [];
  for (const row of rows) {
    const key = row.groupMsgId ?? row.id;
    if (seen.has(key)) continue;
    seen.add(key);
    result.push(row);
  }
  return result;
}

function codePointLength(s: string): number {
  return Array.from(s).length;
}

function utf8ByteLength(s: string): number {
  return new TextEncoder().encode(s).length;
}

function useAliasFromPath(): string | null {
  const pathname = usePathname();
  return useMemo(() => {
    if (!pathname) return null;
    const parts = pathname.split("/").filter(Boolean);
    if (parts.length < 2 || parts[0] !== "chat") return null;
    return decodeURIComponent(parts[1]!);
  }, [pathname]);
}

function LocReqRow({ message, mine }: { message: MessageRow; mine: boolean }) {
  const delivery = Object.values(message.deliveries)[0];
  const expired = isLocReqExpired(message);
  const state =
    expired && delivery?.state !== "fulfilled" ? "expired" : (delivery?.state ?? "queued");
  return (
    <Stack direction="row" sx={{ justifyContent: "center", my: 1 }}>
      <Box sx={{ px: 1.5, py: 0.5, border: 1, borderColor: "divider", borderRadius: 2 }}>
        <Typography variant="caption" color="text.secondary">
          <LocationOnIcon fontSize="inherit" sx={{ verticalAlign: "middle" }} />{" "}
          {mine ? "You requested a location" : "Location requested"} -- {state}
        </Typography>
      </Box>
    </Stack>
  );
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

function LocMessageRow({ message, mine }: { message: MessageRow; mine: boolean }) {
  if (!message.loc) return null;
  return (
    <Stack sx={{ alignItems: mine ? "flex-end" : "flex-start", my: 0.75 }}>
      <LocationCard
        lat={message.loc.lat}
        lon={message.loc.lon}
        accM={message.loc.accM}
        fixTsMs={message.loc.fixTs * 1000}
        src={message.loc.src}
        title={mine ? "Location you shared" : "Location received"}
      />
    </Stack>
  );
}

function ThreadInner({ alias }: { alias: string }) {
  const { me } = useAuth();
  const { aliasToUid, learn, groupByAlias } = useDirectory();
  const router = useRouter();
  const peerUid = aliasToUid(alias);
  // docs/GROUP_CHAT_DESIGN.md §5: `alias` resolves to either a DM peer
  // (above) or a group conversation (below) -- the two namespaces are
  // disjoint server-side (§2), so at most one of `peerUid`/`group` is ever
  // set for a given alias.
  const group = groupByAlias(alias);

  const [messages, setMessages] = useState<MessageRow[]>([]);
  const [pageSize, setPageSize] = useState(PAGE_SIZE_STEP);
  const [device, setDevice] = useState<(DeviceDoc & { id: string }) | null>(null);
  const [recentFixes, setRecentFixes] = useState<LocationFixDoc[]>([]);
  const [allowLocate, setAllowLocate] = useState<boolean | null>(null);

  const [composer, setComposer] = useState("");
  const [sending, setSending] = useState(false);
  const [sendError, setSendError] = useState<string | null>(null);
  const [locateBusy, setLocateBusy] = useState(false);
  const [leaving, setLeaving] = useState(false);
  const [snack, setSnack] = useState<string | null>(null);
  const [showNewMessagesChip, setShowNewMessagesChip] = useState(false);

  const markedReadRef = useRef<Set<string>>(new Set());

  // Scroll bookkeeping for the layout effect below -- docs/V03_PLAN.md §2.
  const listRef = useRef<HTMLDivElement | null>(null);
  // Whether the viewer is (within 80 px of) the bottom right now; a ref, not
  // state, because the scroll handler updates it on every scroll event and
  // nothing here needs a re-render when it changes -- it's read by the
  // layout effect and by `markRead` at the moment a message arrives.
  const atBottomRef = useRef(true);
  const prevSeqRef = useRef<SeqRange | null>(null);
  // Set by the "Load older" button right before it grows `pageSize`
  // (re-subscribing the listener at 174-200 with a bigger `limit`), so the
  // layout effect can diff the scrollHeight from just-before-the-prepend
  // against just-after -- reading it any later would already see the grown
  // list.
  const prevScrollHeightRef = useRef<number | null>(null);
  const reducedMotion = useMediaQuery("(prefers-reduced-motion: reduce)");

  const convKey = group ? group.convKey : me && peerUid ? [me.uid, peerUid].sort().join("_") : null;

  // Thread listener -- docs/SERVER_PLAN.md §7.2: `orderBy(seq, desc)
  // limit(50)`. `pageSize` grows on "load older"; re-subscribing with a
  // bigger limit is simpler and just as correct as a cursor for a household-
  // scale thread (§9.3's low daily volume).
  useEffect(() => {
    if (!convKey || !me) {
      return;
    }
    const db = getFirestoreDb();
    const q = query(
      collection(db, "messages"),
      where("convKey", "==", convKey),
      // Redundant against `convKey` (a convKey is built from exactly these
      // two uids), but load-bearing for `firestore.rules`: a `list` is
      // authorised by an *abstract* pre-check against the query's declared
      // filters alone, before any document is read. `convKey` alone gives
      // the `messages` rule (`uid in resource.data.uids`) nothing to prove
      // itself from, so the whole query 403s -- same failure mode, and same
      // fix, as the `devices`/`locatableBy` queries below.
      where("uids", "array-contains", me.uid),
      orderBy("seq", "desc"),
      limit(pageSize)
    );
    const unsubscribe = onSnapshot(q, (snap) => {
      const rows: MessageRow[] = [];
      snap.forEach((d) => rows.push({ id: d.id, ...(d.data() as MessageDoc) }));
      rows.sort((a, b) => a.seq - b.seq);
      // docs/GROUP_CHAT_DESIGN.md §5: collapse a group message's N-1 copies
      // (one per recipient, shared `groupMsgId`) into one bubble. A no-op
      // for DM/pager rows, which have no `groupMsgId`.
      setMessages(dedupeByGroupMsgId(rows));
    });
    return unsubscribe;
  }, [convKey, me, pageSize]);

  // Peer's pager device (for the location card + "Request location") -- see
  // lib/directory.tsx's module docstring for why the query shape differs by
  // role: an admin can query any device directly; a member can only run a
  // query Firestore can prove is safe, i.e. filtered on their own uid being
  // in `locatableBy`.
  useEffect(() => {
    if (!me || !peerUid) {
      return;
    }
    const db = getFirestoreDb();
    const unsubscribers: Unsubscribe[] = [];
    if (me.role === "admin") {
      unsubscribers.push(
        onSnapshot(query(collection(db, "devices"), where("ownerUid", "==", peerUid)), (snap) => {
          const first = snap.docs[0];
          setDevice(first ? { id: first.id, ...(first.data() as DeviceDoc) } : null);
        })
      );
    } else {
      unsubscribers.push(
        onSnapshot(
          query(collection(db, "devices"), where("locatableBy", "array-contains", me.uid)),
          (snap) => {
            const match = snap.docs.find((d) => (d.data() as DeviceDoc).ownerUid === peerUid);
            setDevice(match ? { id: match.id, ...(match.data() as DeviceDoc) } : null);
          }
        )
      );
    }
    return () => unsubscribers.forEach((u) => u());
  }, [me, peerUid]);

  // Recent location fixes for that device -- newest first. The card shows
  // only the newest as "the" fix; the rest (if any) become LocationMap's
  // faint trail.
  useEffect(() => {
    if (!device) {
      return;
    }
    const db = getFirestoreDb();
    const q = query(
      collection(db, "devices", device.id, "locations"),
      orderBy("createdAt", "desc"),
      limit(RECENT_FIXES_LIMIT)
    );
    const unsubscribe = onSnapshot(q, (snap) => {
      setRecentFixes(snap.docs.map((d) => d.data() as LocationFixDoc));
    });
    return unsubscribe;
  }, [device]);

  // Gated on `device` too (not just `recentFixes`), so switching to a peer
  // with no device, or none at all, drops the previous device's stale fixes
  // instead of the effect above having to reset state synchronously on
  // every `device` change (react-hooks/set-state-in-effect).
  const latestFix = device ? (recentFixes[0] ?? null) : null;
  // Oldest to newest, for LocationMap's trail -- undefined (not just a
  // single-point array) when there's nothing to show, so LocationCard can
  // tell "no trail" from "trail of one".
  const fixTrail = useMemo(
    () =>
      device && recentFixes.length > 1
        ? [...recentFixes].reverse().map((f) => ({ lat: f.lat, lon: f.lon }))
        : undefined,
    [device, recentFixes]
  );

  // allow.locate -- gates the "Request location" button.
  useEffect(() => {
    if (!me || !peerUid) {
      return;
    }
    const db = getFirestoreDb();
    const ref = doc(db, "allow", `${me.uid}_${peerUid}`);
    const unsubscribe = onSnapshot(
      ref,
      (snap) => setAllowLocate(snap.exists() ? (snap.data() as AllowEdgeDoc).locate : false),
      () => setAllowLocate(false)
    );
    return unsubscribe;
  }, [me, peerUid]);

  // Mark-read: any message addressed to me with a not-yet-'read' webapp
  // delivery -- docs/SERVER_PLAN.md §6.3. Gated on the viewer actually being
  // at the bottom of the list *and* the tab being visible
  // (docs/V03_PLAN.md §2 point 4, same predicate as
  // `NotificationWatcher.tsx:56-62`) -- called from the places that can make
  // that gate newly true (the scroll handler reaching bottom, a
  // `visibilitychange` to visible, and every scroll-to-bottom transition
  // below) rather than from a plain `[messages]` effect, so a thread left
  // scrolled up or backgrounded does not mark everything read just because
  // a new message happened to arrive.
  // Plain function, not `useCallback` -- this file's existing handlers
  // (`handleSend`, `handleLocate`, `handleScroll` below) follow the same
  // pattern, and the React Compiler (`eslint-config-next`'s
  // `react-hooks/preserve-manual-memoization`) rejects a hand-written
  // dependency list it disagrees with. It closes over this render's
  // `messages`/`me`/`alias`, so the `visibilitychange` listener below goes
  // through `markReadRef` (the same "latest ref" shape as
  // `NotificationWatcher.tsx`'s `pathnameRef`/`uidToAliasRef`) instead of
  // being listed as an effect dependency, so it's never stale without the
  // listener having to be torn down and re-added on every message.
  function markRead() {
    if (!me) return;
    if (!atBottomRef.current || document.visibilityState !== "visible") return;
    for (const msg of messages) {
      if (msg.recipientUid !== me.uid) continue;
      if (markedReadRef.current.has(msg.id)) continue;
      const webapp = Object.values(msg.deliveries).find((d) => d.kind === "webapp");
      if (!webapp || webapp.state === "read") continue;
      markedReadRef.current.add(msg.id);
      void api.post(`/conversations/${encodeURIComponent(alias)}/messages/${msg.id}/read`).catch(() => {
        markedReadRef.current.delete(msg.id);
      });
    }
  }
  const markReadRef = useRef(markRead);
  useEffect(() => {
    markReadRef.current = markRead;
  });

  useEffect(() => {
    function onVisibilityChange() {
      if (document.visibilityState === "visible") markReadRef.current();
    }
    document.addEventListener("visibilitychange", onVisibilityChange);
    return () => document.removeEventListener("visibilitychange", onVisibilityChange);
  }, []);

  // Scrolls the list to its newest message and clears the "New messages"
  // chip -- the single choke point for every "reached the bottom"
  // transition, so it's also the single choke point for re-checking
  // mark-read (see `markRead` above).
  function scrollToBottom(behavior: ScrollBehavior) {
    const el = listRef.current;
    if (!el) return;
    el.scrollTo({ top: el.scrollHeight, behavior });
    atBottomRef.current = true;
    setShowNewMessagesChip(false);
    markRead();
  }
  // Read through a ref inside the layout effect below (the same shape as
  // `markReadRef` above), so that effect's deps can stay `[messages, me,
  // reducedMotion]` instead of re-running on every render of `ThreadInner`
  // (e.g. every composer keystroke) just because `scrollToBottom` -- a
  // plain, unmemoized function -- is a new reference each time.
  const scrollToBottomRef = useRef(scrollToBottom);
  useEffect(() => {
    scrollToBottomRef.current = scrollToBottom;
  });

  function handleScroll(event: React.UIEvent<HTMLDivElement>) {
    const el = event.currentTarget;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < AT_BOTTOM_THRESHOLD_PX;
    atBottomRef.current = atBottom;
    if (atBottom) {
      setShowNewMessagesChip(false);
      markRead();
    }
  }

  // Auto-scroll -- docs/V03_PLAN.md §2. Runs in the same commit as the DOM
  // update that rendered the new `messages` array, before the browser
  // paints, so a jump-to-bottom or a prepend's scrollTop correction never
  // flashes the wrong position first.
  useIsomorphicLayoutEffect(() => {
    const el = listRef.current;
    const prev = prevSeqRef.current;
    const next = seqRangeOf(messages);

    if (el && next) {
      if (!prev) {
        // First snapshot with data: jump to the bottom, no animation.
        scrollToBottomRef.current("auto");
      } else if (next.last > prev.last) {
        // Append: a new last message. Follow it if the viewer was already
        // at the bottom or it's their own message; otherwise hold position
        // and let the chip offer the jump.
        const lastMessage = messages[messages.length - 1]!;
        if (atBottomRef.current || lastMessage.senderUid === me?.uid) {
          scrollToBottomRef.current(reducedMotion ? "auto" : "smooth");
        } else {
          setShowNewMessagesChip(true);
        }
      } else if (next.first < prev.first) {
        // Prepend ("Load older"): hold the viewer's place by the exact
        // amount the content above them grew. `prevScrollHeightRef` is only
        // cleared here, not on every run, because the re-subscribed
        // listener (174-200) can deliver a cache-then-server pair of
        // snapshots for the same expanded query -- an earlier no-op fire
        // (identical `first`/`last`, neither branch above taken) must not
        // discard the anchor the real prepend snapshot still needs.
        const prevScrollHeight = prevScrollHeightRef.current;
        if (prevScrollHeight !== null) {
          el.scrollTop += el.scrollHeight - prevScrollHeight;
          prevScrollHeightRef.current = null;
        }
      }
    }

    prevSeqRef.current = next;
  }, [messages, me, reducedMotion]);

  const bodyCodePoints = codePointLength(composer);
  const bodyBytes = utf8ByteLength(composer);
  const bodyValid =
    composer.trim().length > 0 &&
    bodyCodePoints <= BODY_MAX_CODEPOINTS &&
    bodyBytes <= BODY_MAX_UTF8_BYTES;

  async function handleSend() {
    if (!bodyValid || sending) return;
    setSending(true);
    setSendError(null);
    try {
      const resp = await api.post<{ id: string }>(
        `/conversations/${encodeURIComponent(alias)}/messages`,
        { body: composer }
      );
      setComposer("");
      // Bootstrap the directory from the message we can always read back --
      // skipped for a group (`!peerUid` is also true there, but there is no
      // single peer to learn: `resp.id` names one arbitrary fan-out copy,
      // and `data.recipientUid` would be one member picked essentially at
      // random, wrongly `learn()`-ing this group's alias as a DM with them).
      if (!peerUid && !group) {
        const snap = await getDoc(doc(getFirestoreDb(), "messages", resp.id));
        if (snap.exists()) {
          const data = snap.data() as MessageDoc;
          learn(data.recipientUid, alias);
        }
      }
    } catch (err) {
      setSendError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to send");
    } finally {
      setSending(false);
    }
  }

  async function handleLocate() {
    setLocateBusy(true);
    try {
      const resp = await api.post<{ requestId: string | null; cached: boolean }>(
        `/conversations/${encodeURIComponent(alias)}/locate`
      );
      setSnack(
        resp.cached
          ? "Answered from a recent fix -- see the location card."
          : "Location requested -- watch the thread for the reply."
      );
    } catch (err) {
      setSnack(err instanceof ApiError ? String(err.detail ?? "Location request failed.") : "Location request failed.");
    } finally {
      setLocateBusy(false);
    }
  }

  // docs/V02_DESIGN.md §5/§4.3: unobtrusive, next to the locate action --
  // absent on older firmware or once the backoff has cleared.
  const locateBackoffLabel = device ? locBackoffLabel(device.status.locBackoffS) : null;

  // docs/GROUP_CHAT_DESIGN.md §5: leave action -> `DELETE
  // /api/conversations/{alias}/members/me`.
  async function handleLeave() {
    if (!group || leaving) return;
    if (!window.confirm(`Leave "${group.name}"? You'll keep read access to messages you already received.`)) {
      return;
    }
    setLeaving(true);
    try {
      await api.del(`/conversations/${encodeURIComponent(alias)}/members/me`);
      router.push("/chat");
    } catch (err) {
      setSnack(err instanceof ApiError ? String(err.detail ?? "Failed to leave group.") : "Failed to leave group.");
    } finally {
      setLeaving(false);
    }
  }

  return (
    <Stack spacing={2} sx={{ height: "calc(100vh - 140px)" }}>
      <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
        <Typography variant="h6">{group ? group.name : `@${alias}`}</Typography>
        {group && (
          <Typography variant="caption" color="text.secondary">
            @{alias}
          </Typography>
        )}
        {device && (
          <Typography variant="caption" color="text.secondary">
            pager: {device.status.state ?? "unknown"}
          </Typography>
        )}
        <Box sx={{ flexGrow: 1 }} />
        {group && (
          <Button size="small" color="inherit" disabled={leaving} onClick={() => void handleLeave()}>
            Leave
          </Button>
        )}
        {allowLocate && (
          <Stack spacing={0.25} sx={{ alignItems: "flex-end" }}>
            <Button
              size="small"
              variant="outlined"
              startIcon={<LocationOnIcon />}
              disabled={locateBusy}
              onClick={() => void handleLocate()}
            >
              Request location
            </Button>
            {locateBackoffLabel && (
              <Typography variant="caption" color="text.secondary">
                {locateBackoffLabel}
              </Typography>
            )}
          </Stack>
        )}
      </Stack>

      {latestFix && (
        <LocationCard
          lat={latestFix.lat}
          lon={latestFix.lon}
          accM={latestFix.accM}
          fixTsMs={latestFix.fixTs * 1000}
          src={latestFix.src}
          cached={latestFix.cached}
          trail={fixTrail}
        />
      )}

      {!peerUid && !group && (
        <Alert severity="info">
          No conversation with @{alias} yet on this account. Send a message below to start one.
        </Alert>
      )}

      {/* `position: relative` lives on this wrapper, not the scrolling Box
          below -- an absolutely positioned child of the scroll container
          itself is positioned against that container's padding box at
          scroll origin and scrolls away WITH the content, which is exactly
          when the chip needs to stay visible. `minHeight: 0` keeps this
          flex child shrinkable so the inner `overflowY: auto` box is the
          one that actually scrolls, not this wrapper. */}
      <Box sx={{ position: "relative", flexGrow: 1, minHeight: 0, display: "flex", flexDirection: "column" }}>
        <Box ref={listRef} onScroll={handleScroll} sx={{ flexGrow: 1, overflowY: "auto", px: 1 }}>
          {peerUid && messages.length >= pageSize && (
            <Stack direction="row" sx={{ justifyContent: "center", mb: 1 }}>
              <Button
                size="small"
                onClick={() => {
                  // Read the pre-prepend scrollHeight now -- the layout
                  // effect that fires once the bigger-limit listener
                  // delivers its next snapshot needs the "before" figure,
                  // and by then the DOM already reflects the "after" one.
                  if (listRef.current) {
                    prevScrollHeightRef.current = listRef.current.scrollHeight;
                  }
                  setPageSize((n) => n + PAGE_SIZE_STEP);
                }}
              >
                Load older
              </Button>
            </Stack>
          )}
          {messages.map((m) => {
            const mine = m.senderUid === me?.uid;
            if (m.kind === "loc_req") return <LocReqRow key={m.id} message={m} mine={mine} />;
            if (m.kind === "loc") return <LocMessageRow key={m.id} message={m} mine={mine} />;
            return <MessageBubble key={m.id} message={m} mine={mine} isGroup={Boolean(group)} />;
          })}
        </Box>
        {showNewMessagesChip && (
          <Chip
            label="New messages ↓"
            color="primary"
            onClick={() => scrollToBottom(reducedMotion ? "auto" : "smooth")}
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

      <Stack direction="row" spacing={1} sx={{ alignItems: "flex-end" }}>
        <TextField
          fullWidth
          multiline
          maxRows={4}
          placeholder="Message"
          value={composer}
          onChange={(e) => setComposer(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && !e.shiftKey) {
              e.preventDefault();
              void handleSend();
            }
          }}
          helperText={`${bodyCodePoints}/${BODY_MAX_CODEPOINTS} code points -- ${bodyBytes}/${BODY_MAX_UTF8_BYTES} bytes`}
          error={bodyCodePoints > BODY_MAX_CODEPOINTS || bodyBytes > BODY_MAX_UTF8_BYTES}
        />
        <Button
          variant="contained"
          endIcon={sending ? <CircularProgress size={16} color="inherit" /> : <SendIcon />}
          disabled={!bodyValid || sending}
          onClick={() => void handleSend()}
        >
          Send
        </Button>
      </Stack>
      {sendError && <Alert severity="error">{sendError}</Alert>}

      <Snackbar
        open={snack !== null}
        autoHideDuration={5000}
        onClose={() => setSnack(null)}
        message={snack}
      />
    </Stack>
  );
}

export default function ThreadPageClient() {
  const alias = useAliasFromPath();
  return (
    <RequireAuth>
      <AppShell>
        {/* `key={alias}` remounts ThreadInner on every alias change, so its
            local state (messages, device, latestFix, allowLocate) always
            starts fresh instead of needing an explicit "clear on dependency
            change" effect -- avoiding a synchronous setState-in-effect. */}
        {alias ? <ThreadInner key={alias} alias={alias} /> : <CircularProgress />}
      </AppShell>
    </RequireAuth>
  );
}

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
import { usePathname } from "next/navigation";
import { useEffect, useMemo, useRef, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Snackbar from "@mui/material/Snackbar";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
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

interface MessageRow extends MessageDoc {
  id: string;
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

function MessageBubble({ message, mine }: { message: MessageRow; mine: boolean }) {
  const atMs = (message.ts ?? 0) * 1000;
  return (
    <Stack sx={{ alignItems: mine ? "flex-end" : "flex-start", my: 0.75 }}>
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
  const { aliasToUid, learn } = useDirectory();
  const peerUid = aliasToUid(alias);

  const [messages, setMessages] = useState<MessageRow[]>([]);
  const [pageSize, setPageSize] = useState(PAGE_SIZE_STEP);
  const [device, setDevice] = useState<(DeviceDoc & { id: string }) | null>(null);
  const [recentFixes, setRecentFixes] = useState<LocationFixDoc[]>([]);
  const [allowLocate, setAllowLocate] = useState<boolean | null>(null);

  const [composer, setComposer] = useState("");
  const [sending, setSending] = useState(false);
  const [sendError, setSendError] = useState<string | null>(null);
  const [locateBusy, setLocateBusy] = useState(false);
  const [snack, setSnack] = useState<string | null>(null);

  const markedReadRef = useRef<Set<string>>(new Set());

  const convKey = me && peerUid ? [me.uid, peerUid].sort().join("_") : null;

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
      setMessages(rows);
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
  // delivery -- docs/SERVER_PLAN.md §6.3.
  useEffect(() => {
    if (!me) return;
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
  }, [messages, me, alias]);

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
      if (!peerUid) {
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

  return (
    <Stack spacing={2} sx={{ height: "calc(100vh - 140px)" }}>
      <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
        <Typography variant="h6">@{alias}</Typography>
        {device && (
          <Typography variant="caption" color="text.secondary">
            pager: {device.status.state ?? "unknown"}
          </Typography>
        )}
        <Box sx={{ flexGrow: 1 }} />
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

      {!peerUid && (
        <Alert severity="info">
          No conversation with @{alias} yet on this account. Send a message below to start one.
        </Alert>
      )}

      <Box sx={{ flexGrow: 1, overflowY: "auto", px: 1 }}>
        {peerUid && messages.length >= pageSize && (
          <Stack direction="row" sx={{ justifyContent: "center", mb: 1 }}>
            <Button size="small" onClick={() => setPageSize((n) => n + PAGE_SIZE_STEP)}>
              Load older
            </Button>
          </Stack>
        )}
        {messages.map((m) => {
          const mine = m.senderUid === me?.uid;
          if (m.kind === "loc_req") return <LocReqRow key={m.id} message={m} mine={mine} />;
          if (m.kind === "loc") return <LocMessageRow key={m.id} message={m} mine={mine} />;
          return <MessageBubble key={m.id} message={m} mine={mine} />;
        })}
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

"use client";

/** `/chat/view/[key]` -- docs/FAMILIES_DESIGN.md §5.2: a family admin's
 * read-only view of a conversation they are not a party to, opened from the
 * "Family" tab on `/chat` (docs/FAMILIES_TASKS.md 2.4). Banner: "You're
 * viewing @kid's conversation with @peer as a family admin." No composer,
 * no mark-read, no locate -- this account has no edge/ownership claim on
 * the conversation, only the family-admin read grant (`firestore.rules`
 * `conversations`/`messages` predicate, relay tasks 1.4/2.2:
 * `role()=='admin' && fam() in familyIds`).
 *
 * The `[key]` segment is read from `usePathname()`, not Next's route
 * `params` -- same static-export placeholder-shell design as
 * `/chat/[alias]` (see `ThreadPageClient.tsx`'s module docstring): Firebase
 * Hosting rewrites every real `/chat/view/<key>` request onto the one
 * prerendered shell (`web/firebase.json`, ordered *before* the `/chat/**`
 * rule, which would otherwise swallow it first).
 *
 * Naming: `conversations/{key}.participants` (docs/FAMILIES_TASKS.md 2.1)
 * is the only source for a peer's alias here -- this admin is not a party
 * to the conversation, so `useDirectory()`'s API-backed cross-family lookup
 * (`GET /api/directory`, scoped to *this account's* edges/conversations)
 * has no reason to include either participant. `useDirectory().byUid()` is
 * used only to tell whether a given uid is a member of *this admin's own*
 * family (the live `users where familyId == fam` listener any family member
 * can read), which is how "kid" (my family) is told apart from "peer".
 */

import { collection, doc, limit, onSnapshot, orderBy, query, where } from "firebase/firestore";
import { usePathname } from "next/navigation";
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import CircularProgress from "@mui/material/CircularProgress";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";
import useMediaQuery from "@mui/material/useMediaQuery";
import VisibilityIcon from "@mui/icons-material/Visibility";

import AppShell from "@/components/AppShell";
import MessageList, { type MessageRow } from "@/components/MessageList";
import RequireAuth from "@/components/RequireAuth";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import { peerLabel } from "@/lib/names";
import type { ConversationDoc, MessageDoc } from "@/lib/types";
import { threadHeightSx } from "@/lib/layout";

const PAGE_SIZE_STEP = 50;
// docs/V03_PLAN.md §2: "within 80 px of the bottom" counts as at-bottom for
// the auto-scroll gate, same threshold as `ThreadPageClient.tsx`.
const AT_BOTTOM_THRESHOLD_PX = 80;

// See `ThreadPageClient.tsx` for why this guards against the `output:
// 'export'` prerender pass.
const useIsomorphicLayoutEffect = typeof window !== "undefined" ? useLayoutEffect : useEffect;

/** Same shape/purpose as `ThreadPageClient.tsx`'s own `SeqRange` -- kept as
 * a separate, smaller copy here rather than a shared import since the two
 * pages' surrounding effects differ (this one has no mark-read, no
 * composer-driven re-renders to guard against). */
interface SeqRange {
  first: number;
  last: number;
}

function seqRangeOf(messages: MessageRow[]): SeqRange | null {
  if (messages.length === 0) return null;
  return { first: messages[0]!.seq, last: messages[messages.length - 1]!.seq };
}

// Same collapsing as `ThreadPageClient.tsx`'s `dedupeByGroupMsgId` -- see
// that file's docstring for why (docs/GROUP_CHAT_DESIGN.md §2/§5).
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

function useKeyFromPath(): string | null {
  const pathname = usePathname();
  return useMemo(() => {
    if (!pathname) return null;
    const parts = pathname.split("/").filter(Boolean);
    if (parts.length < 3 || parts[0] !== "chat" || parts[1] !== "view") return null;
    return decodeURIComponent(parts[2]!);
  }, [pathname]);
}

function MonitorThreadInner({ convKey }: { convKey: string }) {
  const { byUid } = useDirectory();
  const { familyId } = useFamily();

  const [conversation, setConversation] = useState<ConversationDoc | null>(null);
  const [messages, setMessages] = useState<MessageRow[]>([]);
  const [pageSize, setPageSize] = useState(PAGE_SIZE_STEP);
  const [showNewMessagesChip, setShowNewMessagesChip] = useState(false);

  const listRef = useRef<HTMLDivElement | null>(null);
  const atBottomRef = useRef(true);
  const prevSeqRef = useRef<SeqRange | null>(null);
  const prevScrollHeightRef = useRef<number | null>(null);
  const reducedMotion = useMediaQuery("(prefers-reduced-motion: reduce)");

  // The conversation doc itself, for `participants`/`kind`/`name` -- a
  // single-doc `get`/`onSnapshot` is checked directly against
  // `firestore.rules`'s `conversations/{k}` predicate, no query-shape
  // constraint the way a `list` needs (see the `messages` query below).
  useEffect(() => {
    const unsubscribe = onSnapshot(doc(getFirestoreDb(), "conversations", convKey), (snap) => {
      setConversation(snap.exists() ? (snap.data() as ConversationDoc) : null);
    });
    return unsubscribe;
  }, [convKey]);

  useEffect(() => {
    if (!familyId) return;
    const db = getFirestoreDb();
    const q = query(
      collection(db, "messages"),
      where("convKey", "==", convKey),
      // This admin is not `in uids`, so the query carries the family-admin
      // half of `firestore.rules`'s `messages` predicate instead (relay
      // task 2.2: `role()=='admin' && fam() in familyIds`) -- a `list` is
      // authorised by an abstract pre-check against the query's declared
      // filters alone, before any document is read.
      where("familyIds", "array-contains", familyId),
      orderBy("seq", "desc"),
      limit(pageSize)
    );
    const unsubscribe = onSnapshot(q, (snap) => {
      const rows: MessageRow[] = [];
      snap.forEach((d) => rows.push({ id: d.id, ...(d.data() as MessageDoc) }));
      rows.sort((a, b) => a.seq - b.seq);
      setMessages(dedupeByGroupMsgId(rows));
    });
    return unsubscribe;
  }, [convKey, familyId, pageSize]);

  function scrollToBottom(behavior: ScrollBehavior) {
    const el = listRef.current;
    if (!el) return;
    el.scrollTo({ top: el.scrollHeight, behavior });
    atBottomRef.current = true;
    setShowNewMessagesChip(false);
  }
  const scrollToBottomRef = useRef(scrollToBottom);
  useEffect(() => {
    scrollToBottomRef.current = scrollToBottom;
  });

  function handleScroll(event: React.UIEvent<HTMLDivElement>) {
    const el = event.currentTarget;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < AT_BOTTOM_THRESHOLD_PX;
    atBottomRef.current = atBottom;
    if (atBottom) setShowNewMessagesChip(false);
  }

  // Same auto-scroll shape as `ThreadPageClient.tsx`, minus mark-read (this
  // view has none) and the "own message always follows" exception (this
  // account never sends into this conversation).
  useIsomorphicLayoutEffect(() => {
    const el = listRef.current;
    const prev = prevSeqRef.current;
    const next = seqRangeOf(messages);

    if (el && next) {
      if (!prev) {
        scrollToBottomRef.current("auto");
      } else if (next.last > prev.last) {
        if (atBottomRef.current) {
          scrollToBottomRef.current(reducedMotion ? "auto" : "smooth");
        } else {
          setShowNewMessagesChip(true);
        }
      } else if (next.first < prev.first) {
        const prevScrollHeight = prevScrollHeightRef.current;
        if (prevScrollHeight !== null) {
          el.scrollTop += el.scrollHeight - prevScrollHeight;
          prevScrollHeightRef.current = null;
        }
      }
    }

    prevSeqRef.current = next;
  }, [messages, reducedMotion]);

  const isGroup = conversation?.kind === "group";
  const participants = conversation?.participants ?? {};
  const uids = conversation?.uids ?? [];
  // "kid" = whichever party is a member of my own family; falls back to the
  // first uid if neither resolves yet (conversation still loading) or both/
  // neither do (an edge case `firestore.rules` wouldn't have let us reach
  // anyway, since at least one party must share my family for this admin to
  // have this read grant at all).
  const kidUid = uids.find((u) => byUid(u)?.familyId === familyId) ?? uids[0] ?? null;
  const peerUid = uids.find((u) => u !== kidUid) ?? null;
  function label(uid: string | null): string {
    if (!uid) return "someone";
    const p = participants[uid];
    const d = byUid(uid);
    const alias = p?.alias ?? d?.alias;
    if (!alias) return "someone";
    return peerLabel({ alias, displayName: p?.displayName || d?.displayName, kind: p?.kind ?? d?.kind });
  }
  const banner = isGroup
    ? `You're viewing the "${conversation?.name ?? conversation?.alias ?? convKey}" group as a family admin.`
    : `You're viewing ${label(kidUid)}'s conversation with ${label(peerUid)} as a family admin.`;

  return (
    <Stack spacing={2} sx={threadHeightSx}>
      <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
        <VisibilityIcon color="action" fontSize="small" />
        <Typography variant="subtitle2" color="text.secondary">
          {banner}
        </Typography>
        {!conversation && <CircularProgress size={16} />}
      </Stack>

      <MessageList
        messages={messages}
        meUid={kidUid}
        isGroup={isGroup}
        canLoadOlder={messages.length >= pageSize}
        onLoadOlder={() => {
          if (listRef.current) {
            prevScrollHeightRef.current = listRef.current.scrollHeight;
          }
          setPageSize((n) => n + PAGE_SIZE_STEP);
        }}
        listRef={listRef}
        onScroll={handleScroll}
        showNewMessagesChip={showNewMessagesChip}
        onJumpToBottom={() => scrollToBottom(reducedMotion ? "auto" : "smooth")}
      />
    </Stack>
  );
}

export default function MonitorThreadClient() {
  const convKey = useKeyFromPath();
  return (
    <RequireAuth requireRole="admin">
      <AppShell>
        {/* `key={convKey}` remounts on every key change, same convention as
            `ThreadPageClient.tsx`'s `ThreadInner`. */}
        {convKey ? <MonitorThreadInner key={convKey} convKey={convKey} /> : <CircularProgress />}
      </AppShell>
    </RequireAuth>
  );
}

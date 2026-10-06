"use client";

/** `/chat` -- docs/SERVER_PLAN.md §7.2: contact list from `conversations/*`
 * (last message, unread badge) + device online/battery from `devices/*` for
 * pager owners. Both are Firestore listeners, no API call. */

import {
  type Unsubscribe,
  collection,
  onSnapshot,
  query,
  where,
} from "firebase/firestore";
import { useRouter } from "next/navigation";
import { useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Badge from "@mui/material/Badge";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import Divider from "@mui/material/Divider";
import IconButton from "@mui/material/IconButton";
import List from "@mui/material/List";
import ListItem from "@mui/material/ListItem";
import ListItemButton from "@mui/material/ListItemButton";
import ListItemText from "@mui/material/ListItemText";
import Stack from "@mui/material/Stack";
import Tab from "@mui/material/Tab";
import Tabs from "@mui/material/Tabs";
import Typography from "@mui/material/Typography";
import BatteryStdIcon from "@mui/icons-material/BatteryStd";
import GroupsIcon from "@mui/icons-material/Groups";
import LogoutIcon from "@mui/icons-material/Logout";
import PhoneIcon from "@mui/icons-material/Phone";
import VisibilityIcon from "@mui/icons-material/Visibility";
import WifiIcon from "@mui/icons-material/Wifi";
import WifiOffIcon from "@mui/icons-material/WifiOff";

import AppShell from "@/components/AppShell";
import NewChatDialog from "@/components/NewChatDialog";
import NewGroupDialog from "@/components/NewGroupDialog";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import { formatRelativeAge, tsToMillis } from "@/lib/time";
import type { ConversationDoc, DeviceDoc } from "@/lib/types";

interface ConversationRow extends ConversationDoc {
  convKey: string;
  // `null` for a group row -- a group has no single "other" party
  // (docs/GROUP_CHAT_DESIGN.md §5).
  peerUid: string | null;
}

// docs/FAMILIES_TASKS.md 2.4: a "Family" tab row -- a conversation this
// admin is not a party to, named from both ends via `participants`
// (never from `useDirectory()`, which only resolves *this account's own*
// edges/conversations -- see `MonitorThreadClient.tsx`'s module docstring
// for the full reasoning this mirrors).
interface FamilyRow {
  convKey: string;
  label: string;
  /** The peer's E.164 digits when it/either party is an SMS external, for
   * the phone icon + number under the label (docs/FAMILIES_DESIGN.md §5.2). */
  phone?: string;
  lastPreview: string;
  lastMessageAt: ConversationDoc["lastMessageAt"];
}

/** An external's number as digits (no `+`): its alias is a hash now
 * (docs/CONTACT_REQ_DESIGN.md decision 7), so only `phone` carries it. */
function externalDigits(phone: string | null | undefined): string | undefined {
  return phone ? phone.replace(/^\+/, "") : undefined;
}

function ChatListInner() {
  const { me, isFamilyAdmin } = useAuth();
  const { familyId } = useFamily();
  const { byUid } = useDirectory();
  const router = useRouter();
  const [conversations, setConversations] = useState<ConversationRow[]>([]);
  const [familyConversations, setFamilyConversations] = useState<(ConversationDoc & { convKey: string })[]>([]);
  const [device, setDevice] = useState<DeviceDoc | null>(null);
  const [newChatOpen, setNewChatOpen] = useState(false);
  const [newGroupOpen, setNewGroupOpen] = useState(false);
  const [actionError, setActionError] = useState<string | null>(null);
  // docs/FAMILIES_TASKS.md 2.4: the "Family" tab, admins only -- a member
  // never sees it, so its state stays `"mine"` for everyone else.
  const [tab, setTab] = useState<"mine" | "family">("mine");

  useEffect(() => {
    // `familyId` gates the own-device query below (docs/FAMILIES_DESIGN.md
    // §5.3 / firestore.rules `devices/{d}`'s `sameFam(familyId)` clause,
    // which a `list` query must carry a matching `where` for) -- skip until
    // `useFamily()` has resolved a scope.
    if (!me || !familyId) return;
    const db = getFirestoreDb();
    const unsubscribers: Unsubscribe[] = [];

    const convQuery = query(collection(db, "conversations"), where("uids", "array-contains", me.uid));
    unsubscribers.push(
      onSnapshot(convQuery, (snap) => {
        const rows: ConversationRow[] = [];
        snap.forEach((doc) => {
          const data = doc.data() as ConversationDoc;
          // docs/GROUP_CHAT_DESIGN.md §5: a group row has no single "other"
          // party -- its identity is its own `name`/`alias`, not a peer.
          const peerUid =
            data.kind === "group" ? null : (data.uids.find((u) => u !== me.uid) ?? data.uids[0] ?? null);
          rows.push({ ...data, convKey: doc.id, peerUid });
        });
        rows.sort((a, b) => (tsToMillis(b.lastMessageAt) ?? 0) - (tsToMillis(a.lastMessageAt) ?? 0));
        setConversations(rows);
      })
    );

    const deviceQuery = query(
      collection(db, "devices"),
      where("familyId", "==", familyId),
      where("ownerUid", "==", me.uid)
    );
    unsubscribers.push(
      onSnapshot(deviceQuery, (snap) => {
        const first = snap.docs[0];
        setDevice(first ? (first.data() as DeviceDoc) : null);
      })
    );

    return () => unsubscribers.forEach((u) => u());
  }, [me, familyId]);

  // docs/FAMILIES_TASKS.md 2.4: "Family" tab -- every conversation with a
  // member of this family that I am *not* a party to (my own conversations
  // are already covered by the listener above). Admins only: the
  // `firestore.rules` `conversations`/`messages` family-admin clause (relay
  // task 2.2) is what makes this query legal in the first place.
  useEffect(() => {
    if (!me || !isFamilyAdmin || !familyId) return;
    const q = query(collection(getFirestoreDb(), "conversations"), where("familyIds", "array-contains", familyId));
    const unsubscribe = onSnapshot(q, (snap) => {
      const rows: (ConversationDoc & { convKey: string })[] = [];
      snap.forEach((doc) => {
        const data = doc.data() as ConversationDoc;
        if (data.uids.includes(me.uid)) return;
        rows.push({ ...data, convKey: doc.id });
      });
      rows.sort((a, b) => (tsToMillis(b.lastMessageAt) ?? 0) - (tsToMillis(a.lastMessageAt) ?? 0));
      setFamilyConversations(rows);
    });
    return unsubscribe;
  }, [me, isFamilyAdmin, familyId]);

  // docs/FAMILIES_TASKS.md 2.4: name DM peers from the conversation's own
  // `participants` snapshot first (works for any peer, in or out of my
  // family, without a directory lookup), falling back to the live directory
  // for a conversation written before `participants` existed. A DM row with
  // neither is dropped instead of showing a disabled "uid:xxxx" placeholder.
  // A group row is always "resolved" -- its route alias is the
  // conversation's own `alias` field (docs/GROUP_CHAT_DESIGN.md §5).
  const peerLabels = useMemo(
    () =>
      conversations.flatMap((c) => {
        if (c.kind === "group") {
          const label = c.name ?? c.alias ?? c.convKey;
          return [{ ...c, label, routeAlias: c.alias ?? c.convKey, phone: undefined as string | undefined }];
        }
        const peerUid = c.peerUid;
        const participant = peerUid ? c.participants?.[peerUid] : undefined;
        const directoryEntry = peerUid ? byUid(peerUid) : undefined;
        const alias = participant?.alias ?? directoryEntry?.alias;
        if (!alias) return [];
        const kind = participant?.kind ?? directoryEntry?.kind;
        const phone = kind === "external" ? externalDigits(participant?.phone ?? directoryEntry?.phone) : undefined;
        return [{ ...c, label: alias, routeAlias: alias, phone }];
      }),
    [conversations, byUid]
  );

  // docs/FAMILIES_DESIGN.md §5.2: "@kid ↔ @peer" (or the group name), named
  // the same way as `peerLabels` above -- `participants` first, the live
  // family directory (`byUid`) only to tell which uid is *my* family
  // member, never to name the other side (see
  // `MonitorThreadClient.tsx`'s module docstring for why `useDirectory()`'s
  // API-backed half can't resolve a peer I'm not myself connected to).
  const familyRows = useMemo<FamilyRow[]>(
    () =>
      familyConversations.map((c) => {
        if (c.kind === "group") {
          return {
            convKey: c.convKey,
            label: c.name ?? c.alias ?? c.convKey,
            lastPreview: c.lastPreview,
            lastMessageAt: c.lastMessageAt,
          };
        }
        const participants = c.participants ?? {};
        const kidUid = c.uids.find((u) => byUid(u)?.familyId === familyId) ?? c.uids[0];
        const peerUid = c.uids.find((u) => u !== kidUid);
        const kidAlias = (kidUid && (participants[kidUid]?.alias ?? byUid(kidUid)?.alias)) ?? "?";
        const peerEntry = peerUid ? participants[peerUid] : undefined;
        const peerAlias = peerEntry?.alias ?? (peerUid ? byUid(peerUid)?.alias : undefined) ?? "?";
        return {
          convKey: c.convKey,
          label: `@${kidAlias} ↔ @${peerAlias}`,
          phone:
            peerEntry?.kind === "external"
              ? externalDigits(peerEntry.phone ?? (peerUid ? byUid(peerUid)?.phone : undefined))
              : undefined,
          lastPreview: c.lastPreview,
          lastMessageAt: c.lastMessageAt,
        };
      }),
    [familyConversations, byUid, familyId]
  );

  function openConversation(alias: string) {
    const trimmed = alias.trim();
    if (!trimmed) return;
    router.push(`/chat/${encodeURIComponent(trimmed)}`);
  }

  function openMonitor(convKey: string) {
    router.push(`/chat/view/${encodeURIComponent(convKey)}`);
  }

  // docs/GROUP_CHAT_DESIGN.md §5: leave action -> `DELETE
  // /api/conversations/{alias}/members/me`.
  async function leaveGroup(c: { alias?: string; label: string }) {
    if (!c.alias) return;
    if (!window.confirm(`Leave "${c.label}"? You'll keep read access to messages you already received.`)) {
      return;
    }
    setActionError(null);
    try {
      await api.del(`/conversations/${encodeURIComponent(c.alias)}/members/me`);
    } catch (err) {
      setActionError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to leave group");
    }
  }

  return (
    <Stack spacing={3}>
      {device && (
        <Card variant="outlined">
          <CardContent>
            <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
              {device.status.state === "online" ? (
                <WifiIcon color="success" />
              ) : (
                <WifiOffIcon color="disabled" />
              )}
              <Typography variant="body1">
                Your pager ({device.label}): {device.status.state ?? "unknown"}
              </Typography>
              {device.status.battMv != null && (
                <Chip
                  icon={<BatteryStdIcon />}
                  size="small"
                  label={`${(device.status.battMv / 1000).toFixed(2)} V`}
                />
              )}
              {tsToMillis(device.status.updatedAt) !== null && (
                <Typography variant="caption" color="text.secondary">
                  updated {formatRelativeAge(tsToMillis(device.status.updatedAt)!)}
                </Typography>
              )}
            </Stack>
          </CardContent>
        </Card>
      )}

      {isFamilyAdmin && (
        <Tabs value={tab} onChange={(_, v: "mine" | "family") => setTab(v)}>
          <Tab value="mine" label="My chats" />
          <Tab value="family" label="Family" />
        </Tabs>
      )}

      {tab === "mine" ? (
        <>
          <Stack direction="row" spacing={1}>
            <Button variant="contained" onClick={() => setNewChatOpen(true)}>
              New chat
            </Button>
            {isFamilyAdmin && (
              <Button variant="outlined" onClick={() => setNewGroupOpen(true)} sx={{ whiteSpace: "nowrap" }}>
                New group
              </Button>
            )}
          </Stack>

          {actionError && <Alert severity="error">{actionError}</Alert>}

          <Divider />

          {peerLabels.length === 0 && (
            <Alert severity="info">
              No conversations yet. Start one with New chat above -- ask your admin who you&apos;re
              allowed to message.
            </Alert>
          )}

          <List disablePadding>
            {peerLabels.map((c) => {
              const unread = c.unread?.[me?.uid ?? ""] ?? 0;
              const isGroup = c.kind === "group";
              return (
                <ListItem
                  key={c.convKey}
                  disablePadding
                  divider
                  secondaryAction={
                    isGroup ? (
                      <IconButton edge="end" aria-label={`leave ${c.label}`} onClick={() => void leaveGroup(c)}>
                        <LogoutIcon fontSize="small" />
                      </IconButton>
                    ) : undefined
                  }
                >
                  <ListItemButton onClick={() => openConversation(c.routeAlias)}>
                    <ListItemText
                      primary={
                        <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                          {isGroup && <GroupsIcon fontSize="small" color="action" />}
                          {c.phone && <PhoneIcon fontSize="small" color="action" />}
                          <Typography sx={{ fontWeight: unread > 0 ? 700 : 400 }}>
                            {isGroup ? c.label : `@${c.label}`}
                          </Typography>
                          {unread > 0 && <Badge color="primary" badgeContent={unread} />}
                        </Stack>
                      }
                      secondary={
                        <>
                          {c.phone && (
                            <Typography component="span" variant="caption" color="text.secondary" sx={{ display: "block" }}>
                              +{c.phone}
                            </Typography>
                          )}
                          {c.lastPreview || "(no messages yet)"}
                        </>
                      }
                    />
                    {tsToMillis(c.lastMessageAt) !== null && (
                      <Typography variant="caption" color="text.secondary" sx={{ mr: isGroup ? 4 : 0 }}>
                        {formatRelativeAge(tsToMillis(c.lastMessageAt)!)}
                      </Typography>
                    )}
                  </ListItemButton>
                </ListItem>
              );
            })}
          </List>
        </>
      ) : (
        <>
          {familyRows.length === 0 && (
            <Alert severity="info">No conversations from your family with anyone outside it yet.</Alert>
          )}
          <List disablePadding>
            {familyRows.map((r) => (
              <ListItem key={r.convKey} disablePadding divider>
                <ListItemButton onClick={() => openMonitor(r.convKey)}>
                  <VisibilityIcon fontSize="small" color="action" sx={{ mr: 1.5 }} />
                  <ListItemText
                    primary={
                      <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                        {r.phone && <PhoneIcon fontSize="small" color="action" />}
                        <Typography>{r.label}</Typography>
                      </Stack>
                    }
                    secondary={
                      <>
                        {r.phone && (
                          <Typography component="span" variant="caption" color="text.secondary" sx={{ display: "block" }}>
                            +{r.phone}
                          </Typography>
                        )}
                        {r.lastPreview || "(no messages yet)"}
                      </>
                    }
                  />
                  {tsToMillis(r.lastMessageAt) !== null && (
                    <Typography variant="caption" color="text.secondary">
                      {formatRelativeAge(tsToMillis(r.lastMessageAt)!)}
                    </Typography>
                  )}
                </ListItemButton>
              </ListItem>
            ))}
          </List>
        </>
      )}

      <Box sx={{ minHeight: 0 }} />

      <NewChatDialog open={newChatOpen} onClose={() => setNewChatOpen(false)} />
      <NewGroupDialog open={newGroupOpen} onClose={() => setNewGroupOpen(false)} />
    </Stack>
  );
}

export default function ChatListPage() {
  return (
    <RequireAuth>
      <AppShell>
        <ChatListInner />
      </AppShell>
    </RequireAuth>
  );
}

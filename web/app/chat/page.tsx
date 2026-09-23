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
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import BatteryStdIcon from "@mui/icons-material/BatteryStd";
import GroupsIcon from "@mui/icons-material/Groups";
import LogoutIcon from "@mui/icons-material/Logout";
import WifiIcon from "@mui/icons-material/Wifi";
import WifiOffIcon from "@mui/icons-material/WifiOff";

import AppShell from "@/components/AppShell";
import NewGroupDialog from "@/components/NewGroupDialog";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import { formatRelativeAge, tsToMillis } from "@/lib/time";
import type { ConversationDoc, DeviceDoc } from "@/lib/types";

interface ConversationRow extends ConversationDoc {
  convKey: string;
  // `null` for a group row -- a group has no single "other" party
  // (docs/GROUP_CHAT_DESIGN.md §5).
  peerUid: string | null;
}

function ChatListInner() {
  const { me, isAdmin } = useAuth();
  const { uidToAlias } = useDirectory();
  const router = useRouter();
  const [conversations, setConversations] = useState<ConversationRow[]>([]);
  const [device, setDevice] = useState<DeviceDoc | null>(null);
  const [openAlias, setOpenAlias] = useState("");
  const [newGroupOpen, setNewGroupOpen] = useState(false);
  const [actionError, setActionError] = useState<string | null>(null);

  useEffect(() => {
    if (!me) return;
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

    const deviceQuery = query(collection(db, "devices"), where("ownerUid", "==", me.uid));
    unsubscribers.push(
      onSnapshot(deviceQuery, (snap) => {
        const first = snap.docs[0];
        setDevice(first ? (first.data() as DeviceDoc) : null);
      })
    );

    return () => unsubscribers.forEach((u) => u());
  }, [me]);

  // docs/GROUP_CHAT_DESIGN.md §5: a group row is always "resolved" -- its
  // route alias is the conversation's own `alias` field, not something that
  // depends on this browser's best-effort directory (`lib/directory.tsx`).
  const peerLabels = useMemo(
    () =>
      conversations.map((c) => {
        if (c.kind === "group") {
          const label = c.name ?? c.alias ?? c.convKey;
          return { ...c, label, routeAlias: c.alias ?? c.convKey, resolved: true };
        }
        const alias = c.peerUid ? uidToAlias(c.peerUid) : undefined;
        return {
          ...c,
          label: alias ?? (c.peerUid ? `uid:${c.peerUid.slice(0, 8)}` : "unknown"),
          routeAlias: alias ?? "",
          resolved: alias !== undefined,
        };
      }),
    [conversations, uidToAlias]
  );

  function openConversation(alias: string) {
    const trimmed = alias.trim();
    if (!trimmed) return;
    router.push(`/chat/${encodeURIComponent(trimmed)}`);
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

      <Stack direction="row" spacing={1}>
        <TextField
          label="Open conversation by alias"
          size="small"
          fullWidth
          value={openAlias}
          onChange={(e) => setOpenAlias(e.target.value)}
          onKeyDown={(e) => e.key === "Enter" && openConversation(openAlias)}
        />
        <Button variant="contained" onClick={() => openConversation(openAlias)}>
          Open
        </Button>
        {isAdmin && (
          <Button variant="outlined" onClick={() => setNewGroupOpen(true)} sx={{ whiteSpace: "nowrap" }}>
            New group
          </Button>
        )}
      </Stack>

      {actionError && <Alert severity="error">{actionError}</Alert>}

      <Divider />

      {peerLabels.length === 0 && (
        <Alert severity="info">
          No conversations yet. Open one by alias above -- ask your admin who you&apos;re allowed to
          message.
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
              <ListItemButton
                onClick={() => c.resolved && openConversation(c.routeAlias)}
                disabled={!c.resolved}
              >
                <ListItemText
                  primary={
                    <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                      {isGroup && <GroupsIcon fontSize="small" color="action" />}
                      <Typography sx={{ fontWeight: unread > 0 ? 700 : 400 }}>
                        {isGroup ? c.label : c.resolved ? `@${c.label}` : c.label}
                      </Typography>
                      {unread > 0 && <Badge color="primary" badgeContent={unread} />}
                    </Stack>
                  }
                  secondary={
                    c.resolved
                      ? c.lastPreview || "(no messages yet)"
                      : "contact alias unavailable to this account -- ask your admin"
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

      <Box sx={{ minHeight: 0 }} />

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

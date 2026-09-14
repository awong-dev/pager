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
import List from "@mui/material/List";
import ListItemButton from "@mui/material/ListItemButton";
import ListItemText from "@mui/material/ListItemText";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import BatteryStdIcon from "@mui/icons-material/BatteryStd";
import WifiIcon from "@mui/icons-material/Wifi";
import WifiOffIcon from "@mui/icons-material/WifiOff";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import { formatRelativeAge, tsToMillis } from "@/lib/time";
import type { ConversationDoc, DeviceDoc } from "@/lib/types";

interface ConversationRow extends ConversationDoc {
  convKey: string;
  peerUid: string;
}

function ChatListInner() {
  const { me } = useAuth();
  const { uidToAlias } = useDirectory();
  const router = useRouter();
  const [conversations, setConversations] = useState<ConversationRow[]>([]);
  const [device, setDevice] = useState<DeviceDoc | null>(null);
  const [openAlias, setOpenAlias] = useState("");

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
          const peerUid = data.uids.find((u) => u !== me.uid) ?? data.uids[0];
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

  const peerLabels = useMemo(
    () =>
      conversations.map((c) => ({
        ...c,
        label: uidToAlias(c.peerUid) ?? `uid:${c.peerUid.slice(0, 8)}`,
        resolved: uidToAlias(c.peerUid) !== undefined,
      })),
    [conversations, uidToAlias]
  );

  function openConversation(alias: string) {
    const trimmed = alias.trim();
    if (!trimmed) return;
    router.push(`/chat/${encodeURIComponent(trimmed)}`);
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
      </Stack>

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
          return (
            <ListItemButton
              key={c.convKey}
              onClick={() => c.resolved && openConversation(c.label)}
              disabled={!c.resolved}
              divider
            >
              <ListItemText
                primary={
                  <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                    <Typography sx={{ fontWeight: unread > 0 ? 700 : 400 }}>
                      {c.resolved ? `@${c.label}` : c.label}
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
                <Typography variant="caption" color="text.secondary">
                  {formatRelativeAge(tsToMillis(c.lastMessageAt)!)}
                </Typography>
              )}
            </ListItemButton>
          );
        })}
      </List>

      <Box sx={{ minHeight: 0 }} />
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

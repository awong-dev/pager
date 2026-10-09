"use client";

/** `/family/chat?uid=` -- docs/BRIDGE_PHONE_DESIGN.md decision 12: one member's
 * Google Chat / Voice conversations carried by their bridge phone. Subscribed
 * conversations (rename, roster, pause, unsubscribe), conversations the phone
 * has seen but nobody subscribed (subscribe / ignore), and Add by link. Reads
 * `GET /api/family/members/{uid}/chat` on a short poll (bridge data is
 * server-only). Static export: the member comes from `?uid=`, not a segment.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import Alert from "@mui/material/Alert";
import Autocomplete from "@mui/material/Autocomplete";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogContentText from "@mui/material/DialogContentText";
import DialogTitle from "@mui/material/DialogTitle";
import FormControlLabel from "@mui/material/FormControlLabel";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import Snackbar from "@mui/material/Snackbar";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import ChatRosterEditor, { rosterErrors } from "@/components/ChatRosterEditor";
import ChatSubscribeDialog, { type SubscribeTarget } from "@/components/ChatSubscribeDialog";
import RequireAuth from "@/components/RequireAuth";
import { ApiError } from "@/lib/api";
import { NICK_MAX_CODEPOINTS, nickError } from "@/lib/book";
import {
  defaultRoster,
  getChatTab,
  ignoreChat,
  inspectLink,
  isGoogleChatLink,
  patchChat,
  toMs,
  unsubscribeChat,
} from "@/lib/bridges";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";
import { formatRelativeAge } from "@/lib/time";
import type { BridgeConversationRow, ChatTabOut, RosterEntry } from "@/lib/types";

const POLL_MS = 5000;
const LINK_POLL_MS = 3000;
const LINK_POLL_TRIES = 10; // 30 s

function errText(e: unknown, fallback: string): string {
  return e instanceof ApiError ? String(e.detail ?? e.message) : fallback;
}

function targetOf(r: BridgeConversationRow): SubscribeTarget {
  return { bridgeId: r.bridgeId, ref: r.ref, title: r.title ?? "", isGroup: r.isGroup, people: r.people };
}

function lastSeen(r: BridgeConversationRow): string {
  const ms = toMs(r.lastAt);
  const when = ms === null ? "" : formatRelativeAge(ms);
  return [r.lastPreview, when].filter(Boolean).join(" · ");
}

function SourceChip({ r }: { r: BridgeConversationRow }) {
  return (
    <Stack direction="row" spacing={0.5}>
      <Chip size="small" label={r.isGroup ? "Group" : "DM"} />
      <Chip size="small" variant="outlined" label={r.source === "gvoice" ? "Google Voice" : "Google Chat"} />
    </Stack>
  );
}

function ChatInner() {
  const { familyId } = useFamily();
  const { contacts, byUid } = useDirectory();
  const [ownerUid, setOwnerUid] = useState<string | null>(() =>
    typeof window === "undefined" ? null : new URLSearchParams(window.location.search).get("uid")
  );
  const [tab, setTab] = useState<ChatTabOut | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [showIgnored, setShowIgnored] = useState(false);
  const [subscribe, setSubscribe] = useState<SubscribeTarget | null>(null);
  const [toast, setToast] = useState<string | null>(null);

  const [menu, setMenu] = useState<{ el: HTMLElement; row: BridgeConversationRow } | null>(null);
  const [rename, setRename] = useState<{ row: BridgeConversationRow; name: string } | null>(null);
  const [rosterEdit, setRosterEdit] = useState<{ row: BridgeConversationRow; roster: RosterEntry[] } | null>(null);
  const [unsub, setUnsub] = useState<BridgeConversationRow | null>(null);
  const [dialogBusy, setDialogBusy] = useState(false);
  const [dialogError, setDialogError] = useState<string | null>(null);

  const [link, setLink] = useState("");
  const [linkMsg, setLinkMsg] = useState<{ severity: "info" | "error" | "warning"; text: string } | null>(null);
  const [linkBusy, setLinkBusy] = useState(false);
  const alive = useRef(true);
  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  const owners = useMemo(
    () =>
      contacts
        .filter((c) => byUid(c.uid)?.familyId === familyId)
        .map((c) => ({ uid: c.uid, label: `${c.displayName} (@${c.alias})`, alias: c.alias })),
    [contacts, byUid, familyId]
  );
  const owner = owners.find((o) => o.uid === ownerUid) ?? null;

  const load = useCallback(async () => {
    if (!ownerUid) return null;
    try {
      const t = await getChatTab(ownerUid);
      if (alive.current) {
        setTab(t);
        setError(null);
      }
      return t;
    } catch (e) {
      if (alive.current) setError(errText(e, "Could not load Google Chat conversations"));
      return null;
    }
  }, [ownerUid]);

  useEffect(() => {
    if (!ownerUid) return;
    const first = setTimeout(() => void load(), 0);
    const poll = setInterval(() => void load(), POLL_MS);
    return () => {
      clearTimeout(first);
      clearInterval(poll);
    };
  }, [ownerUid, load]);

  function pickOwner(uid: string | null) {
    setOwnerUid(uid);
    setTab(null);
    if (typeof window !== "undefined") {
      const u = new URL(window.location.href);
      if (uid) u.searchParams.set("uid", uid);
      else u.searchParams.delete("uid");
      window.history.replaceState(null, "", u);
    }
  }

  async function runDialog(fn: () => Promise<unknown>, done: () => void) {
    setDialogBusy(true);
    setDialogError(null);
    try {
      await fn();
      done();
      await load();
    } catch (e) {
      setDialogError(errText(e, "Request failed"));
    } finally {
      setDialogBusy(false);
    }
  }

  async function ignore(r: BridgeConversationRow) {
    try {
      await ignoreChat(r.bridgeId, r.ref);
      await load();
    } catch (e) {
      setError(errText(e, "Could not ignore"));
    }
  }

  async function addByLink() {
    const url = link.trim();
    if (!ownerUid || !url) return;
    if (!isGoogleChatLink(url)) {
      setLinkMsg({ severity: "error", text: "Paste a chat.google.com, mail.google.com/chat or voice.google.com link." });
      return;
    }
    setLinkBusy(true);
    setLinkMsg(null);
    try {
      // The member's paired bridge phone (the tab response lists them; the first is asked).
      const mine = (tab?.bridges ?? []).filter((b) => b.paired);
      if (mine.length === 0) {
        setLinkMsg({ severity: "warning", text: "This member has no paired bridge phone. Add one under Family -> Devices." });
        return;
      }
      await inspectLink(mine[0].id, url);
      setLinkMsg({ severity: "info", text: "Asking the phone to open the link..." });
      for (let i = 0; i < LINK_POLL_TRIES; i++) {
        await new Promise((r) => setTimeout(r, LINK_POLL_MS));
        if (!alive.current) return;
        const t = await load();
        const row = t?.seen.find((r) => r.link === url);
        if (row) {
          setLinkMsg(null);
          setLink("");
          setSubscribe(targetOf(row));
          return;
        }
      }
      setLinkMsg({ severity: "warning", text: "The phone did not report that conversation within 30 seconds. Try again." });
    } catch (e) {
      setLinkMsg({ severity: "error", text: errText(e, "Could not inspect the link") });
    } finally {
      if (alive.current) setLinkBusy(false);
    }
  }

  const seenRows = (tab?.seen ?? []).filter((r) => r.status === "seen" || (showIgnored && r.status === "ignored"));

  return (
    <Stack spacing={3}>
      <Typography variant="h5">Google Chat</Typography>

      <Autocomplete
        options={owners}
        value={owner}
        getOptionLabel={(o) => o.label}
        isOptionEqualToValue={(a, b) => a.uid === b.uid}
        onChange={(_, o) => pickOwner(o ? o.uid : null)}
        renderInput={(params) => <TextField {...params} label="Member" />}
        sx={{ maxWidth: 480 }}
      />

      {!ownerUid && <Alert severity="info">Pick a member to manage their Google Chat conversations.</Alert>}
      {error && <Alert severity="error">{error}</Alert>}

      {ownerUid && (
        <>
          <Stack spacing={1}>
            <Typography variant="h6">Subscribed</Typography>
            <TableContainer>
              <Table size="small">
                <TableHead>
                  <TableRow>
                    <TableCell>Name on pager</TableCell>
                    <TableCell>Type</TableCell>
                    <TableCell>People seen</TableCell>
                    <TableCell>Last message</TableCell>
                    <TableCell />
                  </TableRow>
                </TableHead>
                <TableBody>
                  {tab && tab.subscribed.length === 0 && (
                    <TableRow>
                      <TableCell colSpan={5}>
                        <Typography variant="body2" color="text.secondary">
                          Nothing subscribed yet.
                        </Typography>
                      </TableCell>
                    </TableRow>
                  )}
                  {tab?.subscribed.map((r) => (
                    <TableRow key={`${r.bridgeId}/${r.ref}`}>
                      <TableCell>
                        {r.pagerName ?? r.title}
                        <Stack direction="row" spacing={0.5} sx={{ mt: 0.5 }}>
                          {r.onPager === false && <Chip size="small" color="warning" variant="outlined" label="Not on pager" />}
                          {r.status === "paused" && <Chip size="small" label="Paused" />}
                          {r.canReply === false && <Chip size="small" variant="outlined" label="Read only" />}
                        </Stack>
                      </TableCell>
                      <TableCell>
                        <SourceChip r={r} />
                      </TableCell>
                      <TableCell>{r.people.join(", ") || "--"}</TableCell>
                      <TableCell>{lastSeen(r) || "--"}</TableCell>
                      <TableCell>
                        <Button size="small" onClick={(e) => setMenu({ el: e.currentTarget, row: r })}>
                          Manage
                        </Button>
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </TableContainer>
          </Stack>

          <Stack spacing={1}>
            <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
              <Typography variant="h6">Seen, not subscribed</Typography>
              <FormControlLabel
                control={<Switch checked={showIgnored} onChange={(e) => setShowIgnored(e.target.checked)} />}
                label="Show ignored"
              />
            </Stack>
            <TableContainer>
              <Table size="small">
                <TableHead>
                  <TableRow>
                    <TableCell>Conversation</TableCell>
                    <TableCell>Type</TableCell>
                    <TableCell>People</TableCell>
                    <TableCell>Last message</TableCell>
                    <TableCell />
                  </TableRow>
                </TableHead>
                <TableBody>
                  {tab && seenRows.length === 0 && (
                    <TableRow>
                      <TableCell colSpan={5}>
                        <Typography variant="body2" color="text.secondary">
                          No new conversations. Messages from unknown Chat threads appear here and in Alerts.
                        </Typography>
                      </TableCell>
                    </TableRow>
                  )}
                  {seenRows.map((r) => (
                    <TableRow key={`${r.bridgeId}/${r.ref}`}>
                      <TableCell>
                        {r.title || "(untitled)"}
                        {r.status === "ignored" && <Chip size="small" label="Ignored" sx={{ ml: 1 }} />}
                        {r.heldCount > 0 && (
                          <Typography variant="caption" color="text.secondary" sx={{ display: "block" }}>
                            {r.heldCount} {r.heldCount === 1 ? "message" : "messages"} waiting
                          </Typography>
                        )}
                      </TableCell>
                      <TableCell>
                        <SourceChip r={r} />
                      </TableCell>
                      <TableCell>{r.people.join(", ") || "--"}</TableCell>
                      <TableCell>{lastSeen(r) || "--"}</TableCell>
                      <TableCell>
                        <Stack direction="row" spacing={0.5}>
                          <Button size="small" variant="contained" onClick={() => setSubscribe(targetOf(r))}>
                            Subscribe
                          </Button>
                          {r.status !== "ignored" && (
                            <Button size="small" onClick={() => void ignore(r)}>
                              Ignore
                            </Button>
                          )}
                        </Stack>
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </TableContainer>
          </Stack>

          <Stack spacing={1} sx={{ maxWidth: 640 }}>
            <Typography variant="h6">Add by link</Typography>
            <Typography variant="body2" color="text.secondary">
              To start a conversation nobody has posted in yet, paste its Google Chat or Voice link. The phone opens it
              and reports back.
            </Typography>
            <Stack direction="row" spacing={1}>
              <TextField
                fullWidth
                size="small"
                label="Conversation link"
                value={link}
                onChange={(e) => setLink(e.target.value)}
                disabled={linkBusy}
              />
              <Button variant="outlined" disabled={linkBusy || !link.trim()} onClick={() => void addByLink()}>
                Inspect
              </Button>
            </Stack>
            {linkMsg && <Alert severity={linkMsg.severity}>{linkMsg.text}</Alert>}
          </Stack>
        </>
      )}

      <Menu anchorEl={menu?.el} open={menu !== null} onClose={() => setMenu(null)}>
        <MenuItem
          onClick={() => {
            if (menu) setRename({ row: menu.row, name: menu.row.pagerName ?? menu.row.title ?? "" });
            setDialogError(null);
            setMenu(null);
          }}
        >
          Rename
        </MenuItem>
        {menu?.row.isGroup && (
          <MenuItem
            onClick={() => {
              if (menu) {
                const r = menu.row;
                setRosterEdit({ row: r, roster: defaultRoster(r.people, Object.fromEntries((r.roster ?? []).map((x) => [x.name, x.nick]))) });
              }
              setDialogError(null);
              setMenu(null);
            }}
          >
            Edit roster
          </MenuItem>
        )}
        <MenuItem
          onClick={() => {
            const r = menu?.row;
            setMenu(null);
            if (r) {
              void patchChat(r.bridgeId, r.ref, { paused: r.status !== "paused" })
                .then(() => load())
                .catch((e: unknown) => setError(errText(e, "Could not update")));
            }
          }}
        >
          {menu?.row.status === "paused" ? "Resume" : "Pause"}
        </MenuItem>
        <MenuItem
          onClick={() => {
            if (menu) setUnsub(menu.row);
            setDialogError(null);
            setMenu(null);
          }}
        >
          Unsubscribe
        </MenuItem>
      </Menu>

      <Dialog open={rename !== null} onClose={() => setRename(null)} fullWidth maxWidth="xs">
        <DialogTitle>Rename on pager</DialogTitle>
        <DialogContent>
          {dialogError && <Alert severity="error" sx={{ mb: 1 }}>{dialogError}</Alert>}
          {rename && (
            <TextField
              autoFocus
              fullWidth
              margin="dense"
              label="Name on pager"
              value={rename.name}
              onChange={(e) => setRename({ ...rename, name: e.target.value })}
              error={rename.name.trim() === "" || nickError(rename.name) !== null}
              helperText={nickError(rename.name) ?? `${[...rename.name].length}/${NICK_MAX_CODEPOINTS}`}
            />
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRename(null)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={dialogBusy || !rename || rename.name.trim() === "" || nickError(rename.name) !== null}
            onClick={() =>
              rename &&
              void runDialog(
                () => patchChat(rename.row.bridgeId, rename.row.ref, { pagerName: rename.name.trim() }),
                () => setRename(null)
              )
            }
          >
            Save
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={rosterEdit !== null} onClose={() => setRosterEdit(null)} fullWidth maxWidth="sm">
        <DialogTitle>Roster for {rosterEdit?.row.pagerName ?? rosterEdit?.row.title}</DialogTitle>
        <DialogContent>
          {dialogError && <Alert severity="error" sx={{ mb: 1 }}>{dialogError}</Alert>}
          {rosterEdit && (
            <ChatRosterEditor roster={rosterEdit.roster} onChange={(roster) => setRosterEdit({ ...rosterEdit, roster })} />
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRosterEdit(null)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={dialogBusy || !rosterEdit || rosterErrors(rosterEdit.roster).some((e) => e !== null)}
            onClick={() =>
              rosterEdit &&
              void runDialog(
                () => patchChat(rosterEdit.row.bridgeId, rosterEdit.row.ref, { roster: rosterEdit.roster }),
                () => setRosterEdit(null)
              )
            }
          >
            Save
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={unsub !== null} onClose={() => setUnsub(null)}>
        <DialogTitle>Unsubscribe {unsub?.pagerName ?? unsub?.title}?</DialogTitle>
        <DialogContent>
          {dialogError && <Alert severity="error" sx={{ mb: 1 }}>{dialogError}</Alert>}
          <DialogContentText>
            It leaves the pager&apos;s address book. Past messages stay in the history. New messages from the
            conversation will be held for approval again.
          </DialogContentText>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setUnsub(null)}>Cancel</Button>
          <Button
            color="error"
            disabled={dialogBusy}
            onClick={() => unsub && void runDialog(() => unsubscribeChat(unsub.bridgeId, unsub.ref), () => setUnsub(null))}
          >
            Unsubscribe
          </Button>
        </DialogActions>
      </Dialog>

      {ownerUid && (
        <ChatSubscribeDialog
          ownerUid={ownerUid}
          target={subscribe}
          onClose={() => setSubscribe(null)}
          onSubscribed={(res, name) => {
            setSubscribe(null);
            setToast(
              `${name} is on @${owner?.alias ?? "the member"}'s pager; ${res.delivered} waiting ${
                res.delivered === 1 ? "message" : "messages"
              } delivered`
            );
            void load();
          }}
        />
      )}

      <Snackbar open={toast !== null} autoHideDuration={6000} onClose={() => setToast(null)} message={toast ?? ""} />
    </Stack>
  );
}

export default function FamilyChatPage() {
  return (
    <RequireAuth requireRole="admin">
      <AppShell>
        <ChatInner />
      </AppShell>
    </RequireAuth>
  );
}

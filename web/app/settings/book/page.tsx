"use client";

/** `/settings/book` -- docs/ADDRESS_BOOK_DESIGN.md decision 12: the address
 * book of one person (self by default; a family admin can pick any member),
 * with per-owner nicknames. Membership is not edited here -- approvals live
 * in Family -> People. */

import { useMemo, useState } from "react";
import Link from "next/link";
import Alert from "@mui/material/Alert";
import Autocomplete from "@mui/material/Autocomplete";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import EditIcon from "@mui/icons-material/Edit";
import IconButton from "@mui/material/IconButton";
import List from "@mui/material/List";
import ListItem from "@mui/material/ListItem";
import ListItemText from "@mui/material/ListItemText";
import ListSubheader from "@mui/material/ListSubheader";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { formatPhoneDigits } from "@/components/NewChatDialog";
import { ApiError } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import {
  BOOK_GROUP_ORDER,
  type BookEntry,
  type BookResponse,
  NICK_MAX_CODEPOINTS,
  bookGroup,
  nickError,
  reasonText,
  removeAdded,
  saveNick,
  useBook,
} from "@/lib/book";
import { sourceLabel } from "@/lib/bridges";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

interface Owner {
  uid: string;
  label: string;
}

function NickDialog({
  owner,
  entry,
  onClose,
  onSaved,
}: {
  owner: string;
  entry: BookEntry | null;
  onClose: () => void;
  onSaved: () => void;
}) {
  const fullScreen = useFullScreenDialog();
  // Re-keyed by the parent per entry, so the initial value is just the nick.
  const [value, setValue] = useState(entry?.nick ?? "");
  const [busy, setBusy] = useState(false);
  const [failure, setFailure] = useState<string | null>(null);
  const invalid = nickError(value);

  async function submit(next: string) {
    if (!entry?.uid) return;
    setBusy(true);
    setFailure(null);
    try {
      await saveNick(owner, entry.uid, next);
      onSaved();
      onClose();
    } catch (e) {
      setFailure(e instanceof ApiError ? e.message : "Could not save the nickname");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Dialog fullScreen={fullScreen} open={entry !== null} onClose={onClose} fullWidth maxWidth="xs">
      <DialogTitle>Nickname for {entry?.displayName}</DialogTitle>
      <DialogContent>
        <TextField
          autoFocus
          fullWidth
          margin="dense"
          label="Nickname"
          value={value}
          onChange={(e) => setValue(e.target.value)}
          error={invalid !== null}
          helperText={invalid ?? `${[...value].length}/${NICK_MAX_CODEPOINTS}`}
        />
        {failure && (
          <Alert severity="error" sx={{ mt: 1 }}>
            {failure}
          </Alert>
        )}
      </DialogContent>
      <DialogActions>
        {entry?.nick && (
          <Button color="warning" disabled={busy} onClick={() => submit("")} sx={{ mr: "auto" }}>
            Clear
          </Button>
        )}
        <Button onClick={onClose}>Cancel</Button>
        <Button variant="contained" disabled={busy || invalid !== null || !value.trim()} onClick={() => submit(value)}>
          Save
        </Button>
      </DialogActions>
    </Dialog>
  );
}

function BookInner() {
  const { me, isFamilyAdmin } = useAuth();
  const { familyId } = useFamily();
  const { contacts, byUid } = useDirectory();
  // `?uid=` read once, the same lazy way `family-context` reads `?family=`.
  const [ownerUid, setOwnerUid] = useState<string | null>(() =>
    typeof window === "undefined" ? null : new URLSearchParams(window.location.search).get("uid")
  );
  const effectiveOwner = isFamilyAdmin && ownerUid ? ownerUid : (me?.uid ?? "");
  const { data, error, loading, reload } = useBook(effectiveOwner === me?.uid ? undefined : effectiveOwner);
  const [editing, setEditing] = useState<BookEntry | null>(null);
  // The DELETE response is the refreshed book; it wins until the next reload.
  const [local, setLocal] = useState<{ owner: string; data: BookResponse } | null>(null);
  const [removing, setRemoving] = useState<BookEntry | null>(null);
  const [removeError, setRemoveError] = useState<string | null>(null);
  const shown = local && local.owner === effectiveOwner ? local.data : data;

  async function confirmRemove() {
    if (!removing?.uid) return;
    try {
      setLocal({ owner: effectiveOwner, data: await removeAdded(effectiveOwner, removing.uid) });
      setRemoveError(null);
    } catch (err) {
      setRemoveError(err instanceof ApiError ? String(err.detail ?? err.message) : "Could not remove the entry");
    }
    setRemoving(null);
  }

  const owners = useMemo<Owner[]>(() => {
    const rows: Owner[] = [];
    if (me) rows.push({ uid: me.uid, label: `Me (@${me.alias})` });
    for (const c of contacts) {
      if (c.uid === me?.uid) continue;
      if (byUid(c.uid)?.familyId !== familyId) continue;
      rows.push({ uid: c.uid, label: `${c.displayName} (@${c.alias})` });
    }
    return rows;
  }, [contacts, byUid, familyId, me]);

  const grouped = useMemo(() => {
    const out = new Map<string, BookEntry[]>();
    for (const g of BOOK_GROUP_ORDER) out.set(g, []);
    for (const e of shown?.entries ?? []) out.get(bookGroup(e))?.push(e);
    for (const rows of out.values()) rows.sort((a, b) => a.label.localeCompare(b.label));
    return out;
  }, [shown]);

  const selectedOwner = owners.find((o) => o.uid === effectiveOwner) ?? null;

  return (
    <Stack spacing={2} sx={{ maxWidth: 640 }}>
      <Typography variant="h5">Address book</Typography>

      {isFamilyAdmin && (
        <Autocomplete
          options={owners}
          value={selectedOwner}
          getOptionLabel={(o) => o.label}
          isOptionEqualToValue={(a, b) => a.uid === b.uid}
          onChange={(_, o) => setOwnerUid(o ? o.uid : null)}
          renderInput={(params) => <TextField {...params} label="Whose address book" />}
        />
      )}

      {error && <Alert severity="error">{error}</Alert>}
      {removeError && <Alert severity="error">{removeError}</Alert>}
      {loading && !shown && <CircularProgress size={24} />}

      {shown?.truncated && (
        <Alert severity="info">
          This pager shows {shown.pagerCap} of {shown.entries.length} entries. Entries marked Not on pager are
          only reachable from the web.
        </Alert>
      )}

      {shown && shown.entries.length === 0 && <Alert severity="info">This address book is empty.</Alert>}

      {BOOK_GROUP_ORDER.map((g) => {
        const rows = grouped.get(g) ?? [];
        if (rows.length === 0) return null;
        return (
          <List key={g} dense subheader={<ListSubheader disableSticky>{g}</ListSubheader>}>
            {rows.map((e) => {
              const secondary = [
                e.kind === "external" && e.phone
                  ? formatPhoneDigits(e.phone)
                  : e.kind === "group" || e.chat
                    ? ""
                    : `@${e.alias}`,
                e.nick ? `was ${e.displayName}` : "",
              ]
                .filter(Boolean)
                .join(" · ");
              return (
                <ListItem
                  key={`${e.kind}:${e.alias}`}
                  secondaryAction={
                    e.kind !== "group" && e.uid ? (
                      <Stack direction="row" spacing={0.5} sx={{ alignItems: "center" }}>
                        {e.added && (
                          <Button size="small" color="error" onClick={() => setRemoving(e)}>
                            Remove
                          </Button>
                        )}
                        <IconButton edge="end" aria-label={`Edit nickname for ${e.displayName}`} onClick={() => setEditing(e)}>
                          <EditIcon fontSize="small" />
                        </IconButton>
                      </Stack>
                    ) : null
                  }
                >
                  <ListItemText primary={e.label} secondary={secondary || undefined} />
                  {e.chat && (
                    <Chip
                      size="small"
                      variant="outlined"
                      label={sourceLabel(e.chat.source)}
                      sx={{ mr: e.sendable && e.onPager ? 4 : 1 }}
                    />
                  )}
                  {e.added && <Chip size="small" variant="outlined" label="Added from pager" sx={{ mr: 1 }} />}
                  {!e.sendable && <Chip size="small" label={reasonText(e.reason)} sx={{ mr: 4 }} />}
                  {e.sendable && !e.onPager && (
                    <Chip size="small" color="warning" variant="outlined" label="Not on pager" sx={{ mr: 4 }} />
                  )}
                </ListItem>
              );
            })}
          </List>
        );
      })}

      {isFamilyAdmin && (
        <Typography variant="body2">
          Approved people and numbers are managed in <Link href="/family/people">Family → People</Link>.
        </Typography>
      )}

      <NickDialog
        key={editing?.alias ?? "none"}
        owner={effectiveOwner}
        entry={editing}
        onClose={() => setEditing(null)}
        onSaved={() => {
          setLocal(null);
          reload();
        }}
      />

      <Dialog open={!!removing} onClose={() => setRemoving(null)} fullWidth maxWidth="xs">
        <DialogTitle>Remove from address book</DialogTitle>
        <DialogContent>
          <Typography>
            Remove {removing?.label}? It was added from the pager; it disappears from the list unless
            another rule already includes it.
          </Typography>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRemoving(null)}>Cancel</Button>
          <Button color="error" onClick={() => void confirmRemove()}>
            Remove
          </Button>
        </DialogActions>
      </Dialog>
    </Stack>
  );
}

export default function BookPage() {
  return (
    <RequireAuth>
      <AppShell>
        <BookInner />
      </AppShell>
    </RequireAuth>
  );
}

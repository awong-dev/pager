"use client";

/** Admin-only "create group" dialog -- docs/GROUP_CHAT_DESIGN.md §5: "dialog
 * on /chat -- name, alias, checkbox list of existing contacts from the
 * directory; `POST /api/conversations`." Creation is admin-only (design
 * decision 2, §9 G6); the caller (`web/app/chat/page.tsx`) gates rendering
 * on `useAuth().isAdmin`, same convention as every other admin-only dialog
 * in this app (e.g. `web/app/admin/users/page.tsx`'s create dialog). */

import { useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import FormControlLabel from "@mui/material/FormControlLabel";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import { ApiError, api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";

// Same shape as `relay/app/store/users.py`'s `ALIAS_RE`, mirrored in
// `web/app/admin/contacts/page.tsx` -- client-side "is Create enabled yet"
// validation only; the relay is the real authority
// (docs/GROUP_CHAT_DESIGN.md §2: "same regex as a user alias").
const ALIAS_RE = /^[a-z0-9][a-z0-9_-]{0,15}$/;

interface NewGroupDialogProps {
  open: boolean;
  onClose: () => void;
}

export default function NewGroupDialog({ open, onClose }: NewGroupDialogProps) {
  const { me } = useAuth();
  const { contacts } = useDirectory();
  const [name, setName] = useState("");
  const [alias, setAlias] = useState("");
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  function reset() {
    setName("");
    setAlias("");
    setSelected(new Set());
    setError(null);
  }

  function handleClose() {
    reset();
    onClose();
  }

  function toggle(uid: string) {
    setSelected((prev) => {
      const next = new Set(prev);
      if (next.has(uid)) {
        next.delete(uid);
      } else {
        next.add(uid);
      }
      return next;
    });
  }

  const aliasValid = ALIAS_RE.test(alias);
  const formValid = name.trim().length > 0 && aliasValid && selected.size > 0;

  async function createGroup() {
    if (!me || !formValid) return;
    setBusy(true);
    setError(null);
    try {
      // docs/GROUP_CHAT_DESIGN.md §3/§5 don't say whether the relay folds
      // the creating admin into `memberUids` itself, so the web includes it
      // explicitly -- the admin has to be a member to use the group they
      // just made. A relay that already de-dupes/adds `createdBy` makes
      // this redundant, not wrong; see this task's report for the exact
      // assumption.
      const memberUids = Array.from(new Set([me.uid, ...selected]));
      await api.post("/conversations", { name: name.trim(), alias, memberUids });
      handleClose();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to create group");
    } finally {
      setBusy(false);
    }
  }

  const otherContacts = contacts.filter((c) => c.uid !== me?.uid);

  return (
    <Dialog open={open} onClose={handleClose} fullWidth maxWidth="xs">
      <DialogTitle>New group</DialogTitle>
      <DialogContent>
        <Stack spacing={2} sx={{ mt: 1 }}>
          {error && <Alert severity="error">{error}</Alert>}
          <TextField label="Group name" value={name} onChange={(e) => setName(e.target.value)} fullWidth />
          <TextField
            label="Alias"
            value={alias}
            onChange={(e) => setAlias(e.target.value.toLowerCase())}
            helperText="lowercase, e.g. family -- this is the group's @alias, shared with user aliases"
            error={alias.length > 0 && !aliasValid}
            fullWidth
          />
          <Typography variant="subtitle2">Members</Typography>
          {otherContacts.length === 0 ? (
            <Alert severity="info">No other contacts yet.</Alert>
          ) : (
            <Stack spacing={0}>
              {otherContacts.map((c) => (
                <FormControlLabel
                  key={c.uid}
                  control={<Checkbox checked={selected.has(c.uid)} onChange={() => toggle(c.uid)} />}
                  label={`${c.displayName} (@${c.alias})`}
                  sx={{ ml: 0 }}
                />
              ))}
            </Stack>
          )}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={handleClose}>Cancel</Button>
        <Button onClick={() => void createGroup()} disabled={!formValid || busy}>
          Create
        </Button>
      </DialogActions>
    </Dialog>
  );
}

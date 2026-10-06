"use client";

/** `/family/contacts` -- docs/FAMILIES_DESIGN.md §5.4 Contacts,
 * docs/FAMILIES_TASKS.md 3.6: the family's SMS contacts (name, number,
 * approved-for and implied-for members) with add/rename/delete against
 * `GET/POST/PATCH/DELETE /api/family/contacts`, plus a read-only "Linked
 * families" list of cross-family `allow` edges. The relay sends no SMS and
 * holds no SMS threads: the pager texts its own list, and the device page's
 * SMS log is the record. A duplicate name is a 409 shown in the dialog.
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { familyQuery, useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import type { AllowEdgeDoc } from "@/lib/types";

interface ContactRow {
  uid: string;
  alias: string;
  phone: string;
  displayName: string;
  approvedFor: string[];
  impliedFor?: string[];
}

interface LinkedEdge {
  key: string;
  fromUid: string;
  toUid: string;
}

const NAME_HELP = "Must be unique in the family; the pager shows the first 16 characters.";

const emptyAdd = { phone: "", name: "" };

function FamilyContactsInner() {
  const { familyId } = useFamily();
  const { byUid } = useDirectory();
  const [contacts, setContacts] = useState<ContactRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [linkedEdges, setLinkedEdges] = useState<LinkedEdge[]>([]);
  const [addOpen, setAddOpen] = useState(false);
  const [addForm, setAddForm] = useState(emptyAdd);
  const [renaming, setRenaming] = useState<ContactRow | null>(null);
  const [renameValue, setRenameValue] = useState("");
  const [dialogError, setDialogError] = useState<string | null>(null);
  const [deleting, setDeleting] = useState<ContactRow | null>(null);

  async function loadContacts() {
    setLoadError(null);
    try {
      // `GET /api/family/contacts` returns a bare list (relay/tests/test_family_router.py).
      const resp = await api.get<ContactRow[]>(`/family/contacts${familyQuery()}`);
      setContacts(Array.isArray(resp) ? resp : []);
    } catch (err) {
      setLoadError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load contacts"
      );
    }
  }

  useEffect(() => {
    (async () => {
      await loadContacts();
    })();
  }, [familyId]);

  // Linked families -- read-only cross-family `allow` edges (docs/
  // FAMILIES_TASKS.md 3.6): `familyIds` with two distinct entries.
  useEffect(() => {
    if (!familyId) return;
    const db = getFirestoreDb();
    const q = query(collection(db, "allow"), where("familyIds", "array-contains", familyId));
    const unsubscribe = onSnapshot(q, (snap) => {
      const rows: LinkedEdge[] = [];
      snap.forEach((d) => {
        const data = d.data() as AllowEdgeDoc;
        const ids = data.familyIds ?? [];
        if (new Set(ids).size === 2) {
          rows.push({ key: d.id, fromUid: data.fromUid, toUid: data.toUid });
        }
      });
      setLinkedEdges(rows);
    });
    return unsubscribe;
  }, [familyId]);

  function labelFor(uid: string): string {
    const entry = byUid(uid);
    return entry ? `@${entry.alias}` : uid;
  }

  const addValid = addForm.phone.trim() && addForm.name.trim();

  async function addContact() {
    setError(null);
    setDialogError(null);
    try {
      await api.post(`/family/contacts${familyQuery()}`, {
        phone: addForm.phone.trim(),
        name: addForm.name.trim(),
      });
      setAddOpen(false);
      setAddForm(emptyAdd);
      await loadContacts();
    } catch (err) {
      if (err instanceof ApiError && err.status === 409) {
        setDialogError(String(err.detail ?? err.message));
      } else {
        setAddOpen(false);
        setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to add contact");
      }
    }
  }

  function openRename(c: ContactRow) {
    setError(null);
    setDialogError(null);
    setRenaming(c);
    setRenameValue(c.displayName);
  }

  async function saveRename() {
    if (!renaming) return;
    setError(null);
    setDialogError(null);
    try {
      await api.patch(`/family/contacts/${renaming.uid}${familyQuery()}`, { name: renameValue.trim() });
      setRenaming(null);
      await loadContacts();
    } catch (err) {
      if (err instanceof ApiError && err.status === 409) {
        setDialogError(String(err.detail ?? err.message));
      } else {
        setRenaming(null);
        setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to rename contact");
      }
    }
  }

  async function confirmDelete() {
    if (!deleting) return;
    setError(null);
    try {
      await api.del(`/family/contacts/${deleting.uid}${familyQuery()}`);
      setDeleting(null);
      await loadContacts();
    } catch (err) {
      setDeleting(null);
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to delete contact");
    }
  }

  return (
    <Stack spacing={3}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Contacts</Typography>
        <Button
          variant="contained"
          onClick={() => {
            setDialogError(null);
            setAddOpen(true);
          }}
        >
          Add contact
        </Button>
      </Stack>
      <Typography variant="body2" color="text.secondary">
        Contacts are texted by the pager&apos;s own SMS. Approve a contact for a member (People → Approved) to
        put it on their pager.
      </Typography>
      {error && <Alert severity="error">{error}</Alert>}
      {loadError && <Alert severity="error">{loadError}</Alert>}

      <TableContainer sx={{ overflowX: "auto" }}>
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Name</TableCell>
              <TableCell>Number</TableCell>
              <TableCell>Approved for</TableCell>
              <TableCell align="right" />
            </TableRow>
          </TableHead>
          <TableBody>
            {contacts.map((c) => (
              <TableRow key={c.uid} hover>
                <TableCell>{c.displayName}</TableCell>
                <TableCell>{c.phone}</TableCell>
                <TableCell>
                  <Stack direction="row" spacing={0.5} sx={{ flexWrap: "wrap" }}>
                    {(c.approvedFor ?? []).map((uid) => (
                      <Chip key={uid} size="small" label={labelFor(uid)} />
                    ))}
                    {(c.impliedFor ?? []).map((uid) => (
                      <Tooltip key={`i-${uid}`} title="On their pager because their policy is Open">
                        <Chip size="small" variant="outlined" label={`${labelFor(uid)} (policy)`} />
                      </Tooltip>
                    ))}
                  </Stack>
                </TableCell>
                <TableCell align="right">
                  <Button size="small" onClick={() => openRename(c)}>
                    Rename
                  </Button>
                  <Button size="small" color="error" onClick={() => setDeleting(c)}>
                    Delete
                  </Button>
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </TableContainer>

      <Stack spacing={1}>
        <Typography variant="h6">Linked families</Typography>
        <Typography variant="body2" color="text.secondary">
          Ask your superadmin to link a person from another family.
        </Typography>
        {linkedEdges.length === 0 ? (
          <Typography variant="body2" color="text.secondary">
            No cross-family links yet.
          </Typography>
        ) : (
          <TableContainer sx={{ overflowX: "auto" }}>
            <Table size="small">
              <TableHead>
                <TableRow>
                  <TableCell>From</TableCell>
                  <TableCell>To</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {linkedEdges.map((e) => (
                  <TableRow key={e.key}>
                    <TableCell>{labelFor(e.fromUid)}</TableCell>
                    <TableCell>{labelFor(e.toUid)}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </TableContainer>
        )}
      </Stack>

      <Dialog open={addOpen} onClose={() => setAddOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Add contact</DialogTitle>
        <DialogContent>
          {dialogError && (
            <Alert severity="warning" sx={{ mt: 1 }}>
              {dialogError}
            </Alert>
          )}
          <Stack spacing={2} sx={{ mt: 1 }}>
            <TextField
              label="Phone number"
              value={addForm.phone}
              onChange={(e) => setAddForm({ ...addForm, phone: e.target.value })}
              placeholder="+15551234567"
              fullWidth
            />
            <TextField
              label="Name"
              value={addForm.name}
              onChange={(e) => setAddForm({ ...addForm, name: e.target.value })}
              helperText={NAME_HELP}
              fullWidth
            />
          </Stack>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setAddOpen(false)}>Cancel</Button>
          <Button onClick={() => void addContact()} disabled={!addValid}>
            Add
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={!!renaming} onClose={() => setRenaming(null)} fullWidth maxWidth="xs">
        <DialogTitle>Rename contact</DialogTitle>
        <DialogContent>
          {dialogError && (
            <Alert severity="warning" sx={{ mt: 1 }}>
              {dialogError}
            </Alert>
          )}
          <TextField
            label="Name"
            value={renameValue}
            onChange={(e) => setRenameValue(e.target.value)}
            helperText={NAME_HELP}
            fullWidth
            sx={{ mt: 1 }}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRenaming(null)}>Cancel</Button>
          <Button onClick={() => void saveRename()} disabled={!renameValue.trim()}>
            Save
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={!!deleting} onClose={() => setDeleting(null)} fullWidth maxWidth="xs">
        <DialogTitle>Delete contact</DialogTitle>
        <DialogContent>
          <Typography>
            Delete {deleting?.displayName}? It is removed from every pager&apos;s SMS list.
          </Typography>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setDeleting(null)}>Cancel</Button>
          <Button color="error" onClick={() => void confirmDelete()}>
            Delete
          </Button>
        </DialogActions>
      </Dialog>
    </Stack>
  );
}

export default function FamilyContactsPage() {
  return (
    <RequireAuth requireRole="admin">
      <AppShell>
        <FamilyContactsInner />
      </AppShell>
    </RequireAuth>
  );
}

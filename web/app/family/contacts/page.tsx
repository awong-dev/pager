"use client";

/** `/family/contacts` -- docs/FAMILIES_DESIGN.md §5.4 Contacts,
 * docs/FAMILIES_TASKS.md 3.6: the family's externals (name, number, approved
 * for which members, last message where available) with rename/add dialogs
 * against `GET/POST/PATCH /api/family/contacts`, plus a read-only "Linked
 * families" list of cross-family `allow` edges.
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
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { familyQuery, useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import type { AllowEdgeDoc, ConversationDoc } from "@/lib/types";

interface ContactRow {
  uid: string;
  alias: string;
  phone: string;
  displayName: string;
  approvedFor: string[];
}

interface ContactsResponse {
  contacts: ContactRow[];
}

interface LinkedEdge {
  key: string;
  fromUid: string;
  toUid: string;
}

const emptyAdd = { phone: "", name: "" };

function FamilyContactsInner() {
  const { familyId } = useFamily();
  const { byUid } = useDirectory();
  const [contacts, setContacts] = useState<ContactRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [previewByUid, setPreviewByUid] = useState<Map<string, string>>(new Map());
  const [linkedEdges, setLinkedEdges] = useState<LinkedEdge[]>([]);
  const [addOpen, setAddOpen] = useState(false);
  const [addForm, setAddForm] = useState(emptyAdd);
  const [renaming, setRenaming] = useState<ContactRow | null>(null);
  const [renameValue, setRenameValue] = useState("");

  async function loadContacts() {
    setLoadError(null);
    try {
      const resp = await api.get<ContactsResponse>(`/family/contacts${familyQuery()}`);
      setContacts(resp.contacts);
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

  // Best-effort "last message" preview: conversations a family admin can
  // read (docs/FAMILIES_DESIGN.md §3 rules, task 2.2: `familyIds
  // array-contains fam`), matched to a contact by `uids`, latest wins.
  useEffect(() => {
    if (!familyId) return;
    const db = getFirestoreDb();
    const q = query(collection(db, "conversations"), where("familyIds", "array-contains", familyId));
    const unsubscribe = onSnapshot(q, (snap) => {
      const latest = new Map<string, { ts: number; preview: string }>();
      snap.forEach((d) => {
        const data = d.data() as ConversationDoc;
        const ts = data.lastMessageAt ? data.lastMessageAt.toMillis() : 0;
        for (const uid of data.uids) {
          const prev = latest.get(uid);
          if (!prev || ts > prev.ts) {
            latest.set(uid, { ts, preview: data.lastPreview });
          }
        }
      });
      setPreviewByUid(new Map(Array.from(latest.entries()).map(([uid, v]) => [uid, v.preview])));
    });
    return unsubscribe;
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
    try {
      await api.post(`/family/contacts${familyQuery()}`, {
        phone: addForm.phone.trim(),
        name: addForm.name.trim(),
      });
      setAddOpen(false);
      setAddForm(emptyAdd);
      await loadContacts();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to add contact");
    }
  }

  function openRename(c: ContactRow) {
    setError(null);
    setRenaming(c);
    setRenameValue(c.displayName);
  }

  async function saveRename() {
    if (!renaming) return;
    setError(null);
    try {
      await api.patch(`/family/contacts/${renaming.uid}${familyQuery()}`, { name: renameValue.trim() });
      setRenaming(null);
      await loadContacts();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to rename contact");
    }
  }

  return (
    <Stack spacing={3}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Contacts</Typography>
        <Button variant="contained" onClick={() => setAddOpen(true)}>
          Add contact
        </Button>
      </Stack>
      {error && <Alert severity="error">{error}</Alert>}
      {loadError && <Alert severity="error">{loadError}</Alert>}

      <TableContainer sx={{ overflowX: "auto" }}>
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Name</TableCell>
              <TableCell>Number</TableCell>
              <TableCell>Approved for</TableCell>
              <TableCell>Last message</TableCell>
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
                    {c.approvedFor.map((uid) => (
                      <Chip key={uid} size="small" label={labelFor(uid)} />
                    ))}
                  </Stack>
                </TableCell>
                <TableCell>{previewByUid.get(c.uid) ?? "--"}</TableCell>
                <TableCell align="right">
                  <Button size="small" onClick={() => openRename(c)}>
                    Rename
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
          <TextField
            label="Name"
            value={renameValue}
            onChange={(e) => setRenameValue(e.target.value)}
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

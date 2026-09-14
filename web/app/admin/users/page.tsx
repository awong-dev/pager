"use client";

/** `/admin/users` -- docs/SERVER_PLAN.md §7.2: table; create (alias, name,
 * email/phone, role); disable; delete. Against `/api/admin/users`. */

import { collection, onSnapshot } from "firebase/firestore";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import DeleteIcon from "@mui/icons-material/Delete";
import IconButton from "@mui/material/IconButton";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { getFirestoreDb } from "@/lib/firebase";
import type { Role, UserDoc } from "@/lib/types";

interface UserRow extends UserDoc {
  uid: string;
}

const emptyForm = { alias: "", displayName: "", email: "", phone: "", role: "member" as Role };

function AdminUsersInner() {
  const [users, setUsers] = useState<UserRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubscribe = onSnapshot(collection(db, "users"), (snap) => {
      const rows: UserRow[] = [];
      snap.forEach((d) => rows.push({ uid: d.id, ...(d.data() as UserDoc) }));
      rows.sort((a, b) => a.alias.localeCompare(b.alias));
      setUsers(rows);
    });
    return unsubscribe;
  }, []);

  async function createUser() {
    setError(null);
    try {
      await api.post("/admin/users", {
        alias: form.alias,
        displayName: form.displayName,
        email: form.email || null,
        phone: form.phone || null,
        role: form.role,
      });
      setCreateOpen(false);
      setForm(emptyForm);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to create user");
    }
  }

  async function toggleDisabled(u: UserRow) {
    setError(null);
    try {
      await api.patch(`/admin/users/${u.uid}`, { disabled: !u.disabled });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to update user");
    }
  }

  async function deleteUser(u: UserRow) {
    setError(null);
    if (!window.confirm(`Delete @${u.alias}? This does not delete their message history.`)) return;
    try {
      await api.del(`/admin/users/${u.uid}`);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to delete user");
    }
  }

  const formValid = form.alias.trim() && form.displayName.trim() && (form.email.trim() || form.phone.trim());

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Users</Typography>
        <Button variant="contained" onClick={() => setCreateOpen(true)}>
          Create user
        </Button>
      </Stack>
      {error && <Alert severity="error">{error}</Alert>}

      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>Alias</TableCell>
            <TableCell>Name</TableCell>
            <TableCell>Contact</TableCell>
            <TableCell>Role</TableCell>
            <TableCell>Enabled</TableCell>
            <TableCell />
          </TableRow>
        </TableHead>
        <TableBody>
          {users.map((u) => (
            <TableRow key={u.uid}>
              <TableCell>@{u.alias}</TableCell>
              <TableCell>{u.displayName}</TableCell>
              <TableCell>{u.email ?? u.phone ?? "--"}</TableCell>
              <TableCell>
                <Chip size="small" label={u.role} color={u.role === "admin" ? "primary" : "default"} />
              </TableCell>
              <TableCell>
                <Switch checked={!u.disabled} onChange={() => void toggleDisabled(u)} size="small" />
              </TableCell>
              <TableCell>
                <IconButton size="small" onClick={() => void deleteUser(u)} aria-label="delete">
                  <DeleteIcon fontSize="small" />
                </IconButton>
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>

      <Dialog open={createOpen} onClose={() => setCreateOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Create user</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            <TextField
              label="Alias"
              value={form.alias}
              onChange={(e) => setForm({ ...form, alias: e.target.value })}
              helperText="lowercase, e.g. mom"
              fullWidth
            />
            <TextField
              label="Display name"
              value={form.displayName}
              onChange={(e) => setForm({ ...form, displayName: e.target.value })}
              fullWidth
            />
            <TextField
              label="Email"
              value={form.email}
              onChange={(e) => setForm({ ...form, email: e.target.value })}
              fullWidth
            />
            <TextField
              label="Phone (+1XXXXXXXXXX)"
              value={form.phone}
              onChange={(e) => setForm({ ...form, phone: e.target.value })}
              helperText="email or phone is required"
              fullWidth
            />
            <TextField
              select
              label="Role"
              value={form.role}
              onChange={(e) => setForm({ ...form, role: e.target.value as Role })}
              fullWidth
            >
              <MenuItem value="member">member</MenuItem>
              <MenuItem value="admin">admin</MenuItem>
            </TextField>
          </Stack>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setCreateOpen(false)}>Cancel</Button>
          <Button onClick={() => void createUser()} disabled={!formValid}>
            Create
          </Button>
        </DialogActions>
      </Dialog>
    </Stack>
  );
}

export default function AdminUsersPage() {
  return (
    <RequireAuth requireAdmin>
      <AppShell>
        <AdminUsersInner />
      </AppShell>
    </RequireAuth>
  );
}

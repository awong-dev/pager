"use client";

/** `/admin/users` -- docs/SERVER_PLAN.md §7.2: table; create (alias, name,
 * email/phone, role); disable; delete. Against `/api/admin/users`. */

import { collection, onSnapshot } from "firebase/firestore";
import Link from "next/link";
import { useEffect, useMemo, useState } from "react";
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
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import DeleteIcon from "@mui/icons-material/Delete";
import IconButton from "@mui/material/IconButton";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import type { Role, UserDoc } from "@/lib/types";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";
import { responsiveTableSx } from "@/lib/tableSx";

interface UserRow extends UserDoc {
  uid: string;
}

const ALL_FAMILIES = "__all__";

const emptyForm = {
  alias: "",
  displayName: "",
  email: "",
  phone: "",
  role: "member" as Role,
  familyId: "",
};

function AdminUsersInner() {
  const fullScreen = useFullScreenDialog();
  const { familyId: scopeFamilyId, families } = useFamily();
  const [users, setUsers] = useState<UserRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);
  // "All" by default only when the switcher itself has no family selected
  // (docs/FAMILIES_TASKS.md 1.8: "default = the switcher family, 'All'
  // option").
  const [filterFamily, setFilterFamily] = useState<string>(scopeFamilyId ?? ALL_FAMILIES);

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

  function openCreate() {
    // Defaults the dialog's family picker to the switcher's family, without
    // clobbering a family the admin already picked earlier in this session.
    setForm((f) => ({ ...f, familyId: f.familyId || scopeFamilyId || "" }));
    setCreateOpen(true);
  }

  const familyNameById = useMemo(() => new Map(families.map((f) => [f.id, f.name])), [families]);

  const visibleUsers = useMemo(
    () => (filterFamily === ALL_FAMILIES ? users : users.filter((u) => u.familyId === filterFamily)),
    [users, filterFamily]
  );

  async function createUser() {
    setError(null);
    try {
      await api.post("/admin/users", {
        alias: form.alias,
        displayName: form.displayName,
        email: form.email || null,
        phone: form.phone || null,
        role: form.role,
        familyId: form.familyId || null,
      });
      setCreateOpen(false);
      setForm({ ...emptyForm, familyId: scopeFamilyId ?? "" });
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

  const formValid =
    form.alias.trim() &&
    form.displayName.trim() &&
    (form.email.trim() || form.phone.trim()) &&
    (form.role === "super" || form.familyId.trim().length > 0);

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ flexWrap: "wrap", justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Users</Typography>
        <Button variant="contained" onClick={openCreate}>
          Create user
        </Button>
      </Stack>
      <TextField
        select
        size="small"
        label="Family"
        value={filterFamily}
        onChange={(e) => setFilterFamily(e.target.value)}
        sx={{ maxWidth: 240 }}
      >
        <MenuItem value={ALL_FAMILIES}>All families</MenuItem>
        {families.map((f) => (
          <MenuItem key={f.id} value={f.id}>
            {f.name}
          </MenuItem>
        ))}
      </TextField>
      {error && <Alert severity="error">{error}</Alert>}

      <TableContainer sx={responsiveTableSx([3,4,5])}>
        {/* Hidden below md: Contact, SMS number, Family. */}
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Alias</TableCell>
              <TableCell>Name</TableCell>
              <TableCell>Contact</TableCell>
              <TableCell>SMS number</TableCell>
              <TableCell>Family</TableCell>
              <TableCell>Role</TableCell>
              <TableCell>Enabled</TableCell>
              <TableCell />
            </TableRow>
          </TableHead>
          <TableBody>
            {visibleUsers.map((u) => (
              <TableRow key={u.uid}>
                <TableCell>@{u.alias}</TableCell>
                <TableCell>{u.displayName}</TableCell>
                <TableCell>{u.email ?? (u.phone ? `${u.phone} (sign-in)` : "--")}</TableCell>
                <TableCell>
                  {u.kind === "external" ? "--" : (u.smsNumber ?? "none")}
                </TableCell>
                <TableCell>{u.familyId ? (familyNameById.get(u.familyId) ?? u.familyId) : "--"}</TableCell>
                <TableCell>
                  <Chip
                    size="small"
                    label={u.role}
                    color={u.role === "super" ? "secondary" : u.role === "admin" ? "primary" : "default"}
                  />
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
      </TableContainer>

      <Dialog fullScreen={fullScreen} open={createOpen} onClose={() => setCreateOpen(false)} fullWidth maxWidth="xs">
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
              label="Sign-in phone (+1XXXXXXXXXX)"
              value={form.phone}
              onChange={(e) => setForm({ ...form, phone: e.target.value })}
              helperText={
                <>
                  Used to sign in. Texts from this number to a family bridge phone count as this person. To text a number from a pager, add it under{" "}
                  <Link href="/family/contacts">Contacts</Link>. Email or phone is required.
                </>
              }
              fullWidth
            />
            <TextField
              select
              label="Family"
              value={form.familyId}
              onChange={(e) => setForm({ ...form, familyId: e.target.value })}
              fullWidth
            >
              {families.map((f) => (
                <MenuItem key={f.id} value={f.id}>
                  {f.name}
                </MenuItem>
              ))}
            </TextField>
            <TextField
              select
              label="Role"
              value={form.role}
              onChange={(e) => setForm({ ...form, role: e.target.value as Role })}
              fullWidth
            >
              <MenuItem value="member">member</MenuItem>
              <MenuItem value="admin">admin</MenuItem>
              <MenuItem value="super">super</MenuItem>
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
    <RequireAuth requireRole="super">
      <AppShell>
        <AdminUsersInner />
      </AppShell>
    </RequireAuth>
  );
}

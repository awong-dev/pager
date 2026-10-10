"use client";

/** `/family/people` -- docs/FAMILIES_DESIGN.md §5.4 People,
 * docs/FAMILIES_TASKS.md 1.9: table of the family's members plus an "Add
 * person" dialog (`POST /api/family/members`) and a row-click
 * `MemberDrawer`. Policy chips (§2 labels) added by task 3.4.
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import Link from "next/link";
import { useCallback, useEffect, useMemo, useState } from "react";
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

import AppShell from "@/components/AppShell";
import MemberDrawer, { type MemberDrawerMember } from "@/components/MemberDrawer";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { familyQuery, useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import { inboundLabel, outboundLabel } from "@/lib/policy";
import type { DeviceDoc, FamilyDoc, Role, UserDoc } from "@/lib/types";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";
import { responsiveTableSx } from "@/lib/tableSx";

interface MemberRow extends UserDoc {
  uid: string;
}

interface DeviceRow extends DeviceDoc {
  id: string;
}

const emptyForm = { alias: "", displayName: "", email: "", phone: "", role: "member" as Role };

function FamilyPeopleInner() {
  const fullScreen = useFullScreenDialog();
  const { familyId } = useFamily();
  const [members, setMembers] = useState<MemberRow[]>([]);
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);
  const [selected, setSelected] = useState<MemberDrawerMember | null>(null);
  const [familyName, setFamilyName] = useState<string | null>(null);
  const [renameOpen, setRenameOpen] = useState(false);
  const [renameValue, setRenameValue] = useState("");
  const [renameError, setRenameError] = useState<string | null>(null);

  const loadFamily = useCallback(async () => {
    try {
      const fam = await api.get<FamilyDoc>(`/family${familyQuery()}`);
      setFamilyName(fam.name);
    } catch {
      // Header just stays blank; the members table is unaffected.
    }
  }, []);

  useEffect(() => {
    if (!familyId) return;
    let live = true;
    api
      .get<FamilyDoc>(`/family${familyQuery()}`)
      .then((fam) => {
        if (live) setFamilyName(fam.name);
      })
      .catch(() => {
        // Header stays blank; the members table is unaffected.
      });
    return () => {
      live = false;
    };
  }, [familyId]);

  async function saveRename() {
    setRenameError(null);
    try {
      await api.patch(`/family${familyQuery()}`, { name: renameValue.trim() });
      setRenameOpen(false);
      await loadFamily();
    } catch (err) {
      setRenameError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to rename family");
    }
  }

  const renameTrimmed = renameValue.trim();
  const renameValid = renameTrimmed.length >= 1 && [...renameTrimmed].length <= 40;

  useEffect(() => {
    if (!familyId) {
      // No family in scope yet -- leave whatever state already held rather
      // than clearing synchronously in the effect body (same convention as
      // `lib/directory.tsx`'s family listener).
      return;
    }
    const db = getFirestoreDb();
    const unsubMembers = onSnapshot(
      query(collection(db, "users"), where("familyId", "==", familyId)),
      (snap) => {
        const rows: MemberRow[] = [];
        snap.forEach((d) => rows.push({ uid: d.id, ...(d.data() as UserDoc) }));
        rows.sort((a, b) => a.alias.localeCompare(b.alias));
        setMembers(rows);
      }
    );
    const unsubDevices = onSnapshot(
      query(collection(db, "devices"), where("familyId", "==", familyId)),
      (snap) => {
        const rows: DeviceRow[] = [];
        snap.forEach((d) => rows.push({ id: d.id, ...(d.data() as DeviceDoc) }));
        setDevices(rows);
      }
    );
    return () => {
      unsubMembers();
      unsubDevices();
    };
  }, [familyId]);

  const devicesByOwner = useMemo(() => {
    const map = new Map<string, DeviceRow[]>();
    for (const d of devices) {
      const list = map.get(d.ownerUid) ?? [];
      list.push(d);
      map.set(d.ownerUid, list);
    }
    return map;
  }, [devices]);

  async function createMember() {
    setError(null);
    try {
      await api.post(`/family/members${familyQuery()}`, {
        alias: form.alias,
        displayName: form.displayName,
        email: form.email || null,
        phone: form.phone || null,
        role: form.role,
      });
      setCreateOpen(false);
      setForm(emptyForm);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to add person");
    }
  }

  async function toggleDisabled(m: MemberRow) {
    setError(null);
    try {
      await api.patch(`/family/members/${m.uid}${familyQuery()}`, { disabled: !m.disabled });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to update person");
    }
  }

  const formValid = form.alias.trim() && form.displayName.trim() && (form.email.trim() || form.phone.trim());

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ flexWrap: "wrap", justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">People</Typography>
        <Button variant="contained" onClick={() => setCreateOpen(true)}>
          Add person
        </Button>
      </Stack>
      {error && <Alert severity="error">{error}</Alert>}

      <Stack direction="row" spacing={1} useFlexGap sx={{ flexWrap: "wrap", alignItems: "center" }}>
        <Typography variant="body1">Family: {familyName ?? "--"}</Typography>
        <Button
          size="small"
          onClick={() => {
            setRenameValue(familyName ?? "");
            setRenameError(null);
            setRenameOpen(true);
          }}
        >
          Rename
        </Button>
      </Stack>

      <TableContainer sx={responsiveTableSx([4,5,6])}>
        {/* Hidden below md: Sign-in, Devices, Policy (row click opens the member panel). */}
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Name</TableCell>
              <TableCell>Alias</TableCell>
              <TableCell>Role</TableCell>
              <TableCell>Sign-in</TableCell>
              <TableCell>Devices</TableCell>
              <TableCell>Policy</TableCell>
              <TableCell>Enabled</TableCell>
            </TableRow>
          </TableHead>
          <TableBody>
            {members.map((m) => (
              <TableRow key={m.uid} hover sx={{ cursor: "pointer" }} onClick={() => setSelected(m)}>
                <TableCell>{m.displayName}</TableCell>
                <TableCell>@{m.alias}</TableCell>
                <TableCell>
                  <Chip
                    size="small"
                    label={m.role}
                    color={m.role === "admin" || m.role === "super" ? "primary" : "default"}
                  />
                </TableCell>
                <TableCell>{m.email ?? (m.phone ? `${m.phone} (sign-in)` : "--")}</TableCell>
                <TableCell>
                  <Stack direction="row" spacing={0.5} sx={{ flexWrap: "wrap" }}>
                    {(devicesByOwner.get(m.uid) ?? []).map((d) => (
                      <Link
                        key={d.id}
                        href={`/devices/${d.id}`}
                        onClick={(e) => e.stopPropagation()}
                        style={{ textDecoration: "none" }}
                      >
                        <Chip size="small" label={d.label} clickable />
                      </Link>
                    ))}
                  </Stack>
                </TableCell>
                <TableCell>
                  <Chip
                    size="small"
                    variant="outlined"
                    label={`Out: ${outboundLabel(m.policy.out)} · In: ${inboundLabel(m.policy.in)}`}
                  />
                </TableCell>
                <TableCell onClick={(e) => e.stopPropagation()}>
                  <Switch checked={!m.disabled} onChange={() => void toggleDisabled(m)} />
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </TableContainer>

      <Dialog fullScreen={fullScreen} open={createOpen} onClose={() => setCreateOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Add person</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            <TextField
              label="Alias"
              value={form.alias}
              onChange={(e) => setForm({ ...form, alias: e.target.value.toLowerCase() })}
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
                  For signing in only. To text a number from a pager, add it under{" "}
                  <Link href="/family/contacts">Contacts</Link>.
                </>
              }
              fullWidth
            />
            <TextField
              select
              label="Role"
              value={form.role}
              onChange={(e) => setForm({ ...form, role: e.target.value as Role })}
              fullWidth
            >
              <MenuItem value="member">Member</MenuItem>
              <MenuItem value="admin">Admin</MenuItem>
            </TextField>
          </Stack>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setCreateOpen(false)}>Cancel</Button>
          <Button onClick={() => void createMember()} disabled={!formValid}>
            Create
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog fullScreen={fullScreen} open={renameOpen} onClose={() => setRenameOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Rename family</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            <TextField
              autoFocus
              label="Family name"
              value={renameValue}
              onChange={(e) => setRenameValue(e.target.value)}
              helperText="1-40 characters"
              fullWidth
            />
            {renameError && <Alert severity="error">{renameError}</Alert>}
          </Stack>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRenameOpen(false)}>Cancel</Button>
          <Button onClick={() => void saveRename()} disabled={!renameValid}>
            Save
          </Button>
        </DialogActions>
      </Dialog>

      <MemberDrawer member={selected} onClose={() => setSelected(null)} />
    </Stack>
  );
}

export default function FamilyPeoplePage() {
  return (
    <RequireAuth requireRole="admin">
      <AppShell>
        <FamilyPeopleInner />
      </AppShell>
    </RequireAuth>
  );
}

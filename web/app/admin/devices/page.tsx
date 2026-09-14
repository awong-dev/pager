"use client";

/** `/admin/devices` -- docs/SERVER_PLAN.md §7.2/§5.5: create device (shows
 * MQTT credentials once), owner, default recipient, revoke/rotate; last
 * status. Credentials are only ever held in this page's local React state
 * (cleared when the dialog closes) -- never written to Firestore, never
 * persisted to localStorage, per the build brief.
 *
 * **`revoke` has no relay route yet**: `relay/app/store/devices.py` has
 * `revoke_device()`, but `relay/app/routers/admin.py` never mounts it (only
 * `POST /devices`, `GET /devices`, `DELETE /devices/{id}` and
 * `POST /devices/{id}/rotate-credentials` exist). This page still offers a
 * Revoke button (calling the endpoint `POST /api/admin/devices/{id}/revoke`
 * this feature needs) so the UI matches §7.2; the resulting 404 is caught
 * and shown inline rather than pretended away. Adding that route closes it.
 * See web/README.md.
 */

import { collection, onSnapshot } from "firebase/firestore";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import type { DeviceDoc, UserDoc } from "@/lib/types";

interface DeviceRow extends DeviceDoc {
  id: string;
}

interface Credentials {
  mqttUsername: string;
  mqttPassword: string;
}

const emptyForm = { deviceId: "", ownerAlias: "", label: "", defaultToAlias: "" };

function DevicesInner() {
  const { uidToAlias } = useDirectory();
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [users, setUsers] = useState<{ uid: string; alias: string }[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);
  const [credentials, setCredentials] = useState<Credentials | null>(null);
  const [revokeError, setRevokeError] = useState<string | null>(null);

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubDevices = onSnapshot(collection(db, "devices"), (snap) => {
      const rows: DeviceRow[] = [];
      snap.forEach((d) => rows.push({ id: d.id, ...(d.data() as DeviceDoc) }));
      setDevices(rows);
    });
    const unsubUsers = onSnapshot(collection(db, "users"), (snap) => {
      const rows: { uid: string; alias: string }[] = [];
      snap.forEach((d) => rows.push({ uid: d.id, alias: (d.data() as UserDoc).alias }));
      setUsers(rows);
    });
    return () => {
      unsubDevices();
      unsubUsers();
    };
  }, []);

  async function createDevice() {
    setError(null);
    try {
      const resp = await api.post<{ device: DeviceRow; mqttUsername: string; mqttPassword: string }>(
        "/admin/devices",
        {
          deviceId: form.deviceId,
          ownerAlias: form.ownerAlias,
          label: form.label,
          defaultToAlias: form.defaultToAlias || null,
        }
      );
      setCreateOpen(false);
      setForm(emptyForm);
      setCredentials({ mqttUsername: resp.mqttUsername, mqttPassword: resp.mqttPassword });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to create device");
    }
  }

  async function rotate(deviceId: string) {
    setError(null);
    try {
      const resp = await api.post<Credentials>(`/admin/devices/${deviceId}/rotate-credentials`);
      setCredentials(resp);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to rotate credentials");
    }
  }

  async function revoke(deviceId: string) {
    setRevokeError(null);
    try {
      await api.post(`/admin/devices/${deviceId}/revoke`);
    } catch (err) {
      if (err instanceof ApiError && err.status === 404) {
        setRevokeError(
          "The revoke endpoint is not implemented server-side yet -- see web/README.md."
        );
      } else {
        setRevokeError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to revoke device");
      }
    }
  }

  async function deleteDevice(deviceId: string) {
    setError(null);
    if (!window.confirm(`Delete device ${deviceId}? This cannot be undone.`)) return;
    try {
      await api.del(`/admin/devices/${deviceId}`);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to delete device");
    }
  }

  const formValid = form.deviceId.trim() && form.ownerAlias.trim() && form.label.trim();

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Devices</Typography>
        <Button variant="contained" onClick={() => setCreateOpen(true)}>
          Create device
        </Button>
      </Stack>
      {error && <Alert severity="error">{error}</Alert>}
      {revokeError && <Alert severity="warning">{revokeError}</Alert>}

      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>Device ID</TableCell>
            <TableCell>Label</TableCell>
            <TableCell>Owner</TableCell>
            <TableCell>Default to</TableCell>
            <TableCell>Status</TableCell>
            <TableCell>Revoked</TableCell>
            <TableCell />
          </TableRow>
        </TableHead>
        <TableBody>
          {devices.map((d) => (
            <TableRow key={d.id}>
              <TableCell>{d.id}</TableCell>
              <TableCell>{d.label}</TableCell>
              <TableCell>@{uidToAlias(d.ownerUid) ?? d.ownerUid.slice(0, 8)}</TableCell>
              <TableCell>{d.defaultToUid ? `@${uidToAlias(d.defaultToUid) ?? d.defaultToUid.slice(0, 8)}` : "--"}</TableCell>
              <TableCell>{d.status?.state ?? "unknown"}</TableCell>
              <TableCell>{d.revokedAt ? "yes" : "no"}</TableCell>
              <TableCell>
                <Stack direction="row" spacing={1}>
                  <Button size="small" onClick={() => void rotate(d.id)}>
                    Rotate
                  </Button>
                  <Button size="small" color="warning" onClick={() => void revoke(d.id)}>
                    Revoke
                  </Button>
                  <Button size="small" color="error" onClick={() => void deleteDevice(d.id)}>
                    Delete
                  </Button>
                </Stack>
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>

      <Dialog open={createOpen} onClose={() => setCreateOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Create device</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            <TextField
              label="Device ID"
              value={form.deviceId}
              onChange={(e) => setForm({ ...form, deviceId: e.target.value })}
              fullWidth
            />
            <TextField
              label="Label"
              value={form.label}
              onChange={(e) => setForm({ ...form, label: e.target.value })}
              fullWidth
            />
            <TextField
              select
              label="Owner"
              value={form.ownerAlias}
              onChange={(e) => setForm({ ...form, ownerAlias: e.target.value })}
              fullWidth
            >
              {users.map((u) => (
                <MenuItem key={u.uid} value={u.alias}>
                  @{u.alias}
                </MenuItem>
              ))}
            </TextField>
            <TextField
              select
              label="Default recipient (optional)"
              value={form.defaultToAlias}
              onChange={(e) => setForm({ ...form, defaultToAlias: e.target.value })}
              fullWidth
            >
              <MenuItem value="">(none -- broadcast to all allowed)</MenuItem>
              {users.map((u) => (
                <MenuItem key={u.uid} value={u.alias}>
                  @{u.alias}
                </MenuItem>
              ))}
            </TextField>
          </Stack>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setCreateOpen(false)}>Cancel</Button>
          <Button onClick={() => void createDevice()} disabled={!formValid}>
            Create
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={credentials !== null} onClose={() => setCredentials(null)} fullWidth maxWidth="xs">
        <DialogTitle>MQTT credentials -- shown once</DialogTitle>
        <DialogContent>
          <Alert severity="warning" sx={{ mb: 2 }}>
            This password is shown only now and is not stored anywhere retrievable -- copy it to the
            device&apos;s provisioning now.
          </Alert>
          {credentials && (
            <Stack spacing={1}>
              <TextField label="Username" value={credentials.mqttUsername} slotProps={{ input: { readOnly: true } }} fullWidth />
              <TextField label="Password" value={credentials.mqttPassword} slotProps={{ input: { readOnly: true } }} fullWidth />
            </Stack>
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setCredentials(null)}>Done</Button>
        </DialogActions>
      </Dialog>
    </Stack>
  );
}

export default function DevicesPage() {
  return (
    <RequireAuth requireAdmin>
      <AppShell>
        <DevicesInner />
      </AppShell>
    </RequireAuth>
  );
}

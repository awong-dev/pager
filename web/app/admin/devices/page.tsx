"use client";

/** `/admin/devices` -- docs/SERVER_PLAN.md §7.2/§5.5, docs/DEVICE_PLAN.md
 * §3.2: create device (Add device wizard), rotate, revoke, last status.
 *
 * `POST /admin/devices` and `POST /admin/devices/{id}/rotate-credentials`
 * (`relay/app/routers/admin.py`'s `DeviceSetupCodeResponse`, S2.2) return a
 * one-time `setupCode` string -- never the plaintext MQTT password or HMAC
 * key, which leave the relay exactly once inside the encrypted bootstrap
 * bundle a real device fetches over its own bootstrap MQTT hop. This page
 * holds the response only in local React state (`setupResult`, cleared when
 * the panel closes) -- never written to Firestore, never persisted to
 * localStorage.
 *
 * `POST /admin/devices/{id}/revoke` is mounted (docs/DEVICE_PLAN.md §3.5,
 * S2.2) and also deletes the device's broker credential.
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

import SetupCodePanel, { type SetupCodeResult } from "./SetupCodePanel";

// `devices/{d}.provisionState` (docs/DEVICE_PLAN.md §3.2/§D0.2) is not yet
// part of `lib/types.ts`'s `DeviceDoc` mirror; declared locally here rather
// than editing that shared file, which is outside this task's `Files` list.
type ProvisionState = "issued" | "provisioned" | null | undefined;

interface DeviceRow extends DeviceDoc {
  id: string;
  provisionState?: ProvisionState;
}

const emptyForm = { deviceId: "", ownerAlias: "", label: "", defaultToAlias: "" };

// `relay/app/routers/admin.py`'s `DeviceSetupCodeResponse` (S2.2), shared by
// `POST /devices` and `POST /devices/{id}/rotate-credentials`; only the
// fields this page reads are declared.
interface DeviceSetupResponse {
  device: { id: string };
  setupCode: string;
  expiresAt: string;
  brokerPush: "pushed" | "manual";
  manualAcl?: string[] | null;
}

function DevicesInner() {
  const { uidToAlias } = useDirectory();
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [users, setUsers] = useState<{ uid: string; alias: string }[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);
  const [setupResult, setSetupResult] = useState<SetupCodeResult | null>(null);

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubDevices = onSnapshot(collection(db, "devices"), (snap) => {
      const rows: DeviceRow[] = [];
      snap.forEach((d) => {
        const data = d.data() as DeviceDoc & { provisionState?: ProvisionState };
        rows.push({ id: d.id, ...data });
      });
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
      const resp = await api.post<DeviceSetupResponse>("/admin/devices", {
        deviceId: form.deviceId,
        ownerAlias: form.ownerAlias,
        label: form.label,
        defaultToAlias: form.defaultToAlias || null,
      });
      setCreateOpen(false);
      setForm(emptyForm);
      setSetupResult({
        deviceId: resp.device.id,
        label: form.label,
        setupCode: resp.setupCode,
        expiresAt: resp.expiresAt,
        brokerPush: resp.brokerPush,
        manualAcl: resp.manualAcl,
      });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to create device");
    }
  }

  async function rotate(device: DeviceRow) {
    setError(null);
    try {
      const resp = await api.post<DeviceSetupResponse>(
        `/admin/devices/${device.id}/rotate-credentials`
      );
      setSetupResult({
        deviceId: device.id,
        label: device.label,
        setupCode: resp.setupCode,
        expiresAt: resp.expiresAt,
        brokerPush: resp.brokerPush,
        manualAcl: resp.manualAcl,
      });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to rotate credentials");
    }
  }

  async function revoke(deviceId: string) {
    setError(null);
    if (!window.confirm(`Revoke device ${deviceId}? This deletes its broker credential immediately.`)) {
      return;
    }
    try {
      await api.post(`/admin/devices/${deviceId}/revoke`);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to revoke device");
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
  const setupResultDevice = setupResult
    ? devices.find((d) => d.id === setupResult.deviceId)
    : undefined;
  const setupResultOnline = setupResultDevice?.provisionState === "provisioned";

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Devices</Typography>
        <Button variant="contained" onClick={() => setCreateOpen(true)}>
          Add device
        </Button>
      </Stack>
      {error && <Alert severity="error">{error}</Alert>}

      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>Device ID</TableCell>
            <TableCell>Label</TableCell>
            <TableCell>Owner</TableCell>
            <TableCell>Default to</TableCell>
            <TableCell>Status</TableCell>
            <TableCell>Provisioned</TableCell>
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
              <TableCell>{d.provisionState === "provisioned" ? "online" : d.provisionState ?? "unknown"}</TableCell>
              <TableCell>{d.revokedAt ? "yes" : "no"}</TableCell>
              <TableCell>
                <Stack direction="row" spacing={1}>
                  <Button size="small" onClick={() => void rotate(d)}>
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
        <DialogTitle>Add device</DialogTitle>
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

      {setupResult && (
        <SetupCodePanel
          result={setupResult}
          online={setupResultOnline}
          onClose={() => setSetupResult(null)}
        />
      )}
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

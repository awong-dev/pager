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
 *
 * **Lock controls (docs/DEVICE_PLAN.md §5.8, docs/DEVICE_TASKS.md W4.4):**
 * "Clear passcode" and the auto-lock select both call `POST /admin/devices/
 * {id}/cfg` (`relay/app/routers/admin.py`'s `PushCfgRequest{lock:{clear?,
 * auto?}}`). The select's current value is read from the device's raw
 * `pendingCfg.obj.cfg.lock.auto` field (`relay/app/devcfg.py`'s
 * `push_cfg`/`_set_pending`) -- the *last requested* auto-lock minutes, not
 * necessarily yet acked by the device (§5.8: `cfg` is "acked `shown` on
 * apply"). `pendingCfg` is, like `provisionState` above, not part of
 * `lib/types.ts`'s `DeviceDoc` mirror -- read straight off the Firestore
 * snapshot instead of adding it there, outside this task's `Files` list.
 */

import { collection, onSnapshot } from "firebase/firestore";
import Link from "next/link";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogContentText from "@mui/material/DialogContentText";
import DialogTitle from "@mui/material/DialogTitle";
import MenuItem from "@mui/material/MenuItem";
import Snackbar from "@mui/material/Snackbar";
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
import DeviceTrustChip from "@/components/DeviceTrustChip";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { locBackoffLabel } from "@/lib/deviceTrust";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import type { DeviceDoc, UserDoc } from "@/lib/types";

import SetupCodePanel, { type SetupCodeResult } from "./SetupCodePanel";

// docs/V02_DESIGN.md §4.4: `POST /api/admin/devices/{id}/ca`
// `{"action":"push"|"unpin"}` (`relay/app/routers/admin.py`'s `push_ca`).
type CaAction = "push" | "unpin";

interface CaConfirmState {
  deviceId: string;
  label: string;
  action: CaAction;
}

// `devices/{d}.provisionState` (docs/DEVICE_PLAN.md §3.2/§D0.2) is not yet
// part of `lib/types.ts`'s `DeviceDoc` mirror; declared locally here rather
// than editing that shared file, which is outside this task's `Files` list.
type ProvisionState = "issued" | "provisioned" | null | undefined;

// `devices/{d}.pendingCfg` -- `relay/app/devcfg.py`'s `_set_pending` shape,
// same "not in `DeviceDoc` yet" situation as `ProvisionState` above.
interface PendingCfgDoc {
  id: string;
  obj: { cfg?: { lock?: { auto?: number; clear?: boolean } } };
  acked: boolean;
}

interface DeviceRow extends DeviceDoc {
  id: string;
  provisionState?: ProvisionState;
  pendingCfg?: PendingCfgDoc;
}

const emptyForm = { deviceId: "", ownerAlias: "", label: "", defaultToAlias: "" };

// docs/DEVICE_PLAN.md §5.8: `auto_min` is a `u8` minutes value, 0 = never;
// default 5. This is the same small fixed menu a parent needs, not a free
// numeric field.
const AUTO_LOCK_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Off" },
  { value: 5, label: "5 min" },
  { value: 15, label: "15 min" },
  { value: 30, label: "30 min" },
  { value: 60, label: "60 min" },
];

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
  const [caConfirm, setCaConfirm] = useState<CaConfirmState | null>(null);
  const [caBusy, setCaBusy] = useState(false);
  const [caError, setCaError] = useState<string | null>(null);
  const [caSuccess, setCaSuccess] = useState<string | null>(null);

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubDevices = onSnapshot(collection(db, "devices"), (snap) => {
      const rows: DeviceRow[] = [];
      snap.forEach((d) => {
        const data = d.data() as DeviceDoc & {
          provisionState?: ProvisionState;
          pendingCfg?: PendingCfgDoc;
        };
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

  async function setAutoLock(deviceId: string, minutes: number) {
    setError(null);
    try {
      await api.post(`/admin/devices/${deviceId}/cfg`, { lock: { auto: minutes } });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to set auto-lock");
    }
  }

  async function clearPasscode(deviceId: string) {
    setError(null);
    if (!window.confirm(`Clear ${deviceId}'s passcode? The pager unlocks immediately.`)) return;
    try {
      await api.post(`/admin/devices/${deviceId}/cfg`, { lock: { clear: true } });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to clear passcode");
    }
  }

  // docs/V02_DESIGN.md §4.4: the change is asynchronous -- the chip only
  // updates once the pager's next `/status` arrives, so success here says
  // exactly that rather than implying it already happened.
  async function submitCaAction() {
    if (!caConfirm) return;
    setCaBusy(true);
    setCaError(null);
    try {
      await api.post(`/admin/devices/${caConfirm.deviceId}/ca`, { action: caConfirm.action });
      setCaConfirm(null);
      setCaSuccess("Sent to the pager; it applies on its next connection.");
    } catch (err) {
      setCaError(
        err instanceof ApiError
          ? String(err.detail ?? err.message)
          : "Failed to update CA trust for this device."
      );
    } finally {
      setCaBusy(false);
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

      <TableContainer sx={{ overflowX: "auto" }}>
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
              <TableCell>CA trust</TableCell>
              <TableCell>Lock</TableCell>
              <TableCell />
            </TableRow>
          </TableHead>
          <TableBody>
            {devices.map((d) => {
              const backoff = locBackoffLabel(d.status?.locBackoffS);
              return (
                <TableRow key={d.id}>
                  <TableCell>{d.id}</TableCell>
                  <TableCell>{d.label}</TableCell>
                  <TableCell>@{uidToAlias(d.ownerUid) ?? d.ownerUid.slice(0, 8)}</TableCell>
                  <TableCell>{d.defaultToUid ? `@${uidToAlias(d.defaultToUid) ?? d.defaultToUid.slice(0, 8)}` : "--"}</TableCell>
                  <TableCell>
                    {d.status?.state ?? "unknown"}
                    {backoff && (
                      <Typography variant="caption" color="text.secondary" component="div">
                        {backoff}
                      </Typography>
                    )}
                  </TableCell>
                  <TableCell>{d.provisionState === "provisioned" ? "online" : d.provisionState ?? "unknown"}</TableCell>
                  <TableCell>{d.revokedAt ? "yes" : "no"}</TableCell>
                  <TableCell>
                    <Stack spacing={0.5} sx={{ alignItems: "flex-start" }}>
                      <DeviceTrustChip tls={d.status?.tls} caFp={d.status?.caFp} />
                      <Stack direction="row" spacing={0.5}>
                        <Button
                          size="small"
                          onClick={() =>
                            setCaConfirm({ deviceId: d.id, label: d.label, action: "push" })
                          }
                        >
                          Push CA
                        </Button>
                        <Button
                          size="small"
                          color="warning"
                          onClick={() =>
                            setCaConfirm({ deviceId: d.id, label: d.label, action: "unpin" })
                          }
                        >
                          Un-pin CA
                        </Button>
                      </Stack>
                    </Stack>
                  </TableCell>
                  <TableCell>
                    <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                      <TextField
                        select
                        size="small"
                        label="Auto-lock"
                        value={d.pendingCfg?.obj.cfg?.lock?.auto ?? ""}
                        onChange={(e) => void setAutoLock(d.id, Number(e.target.value))}
                        sx={{ minWidth: 100 }}
                      >
                        {AUTO_LOCK_OPTIONS.map((o) => (
                          <MenuItem key={o.value} value={o.value}>
                            {o.label}
                          </MenuItem>
                        ))}
                      </TextField>
                      <Button size="small" onClick={() => void clearPasscode(d.id)}>
                        Clear passcode
                      </Button>
                    </Stack>
                  </TableCell>
                  <TableCell>
                    <Stack direction="row" spacing={1} sx={{ flexWrap: "wrap" }}>
                      <Button size="small" component={Link} href={`/devices/${d.id}`}>
                        SMS
                      </Button>
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
              );
            })}
          </TableBody>
        </Table>
      </TableContainer>

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

      <Dialog open={caConfirm !== null} onClose={() => (caBusy ? undefined : setCaConfirm(null))}>
        <DialogTitle>
          {caConfirm?.action === "push" ? "Push CA certificate" : "Un-pin CA certificate"}
        </DialogTitle>
        <DialogContent>
          {caError && (
            <Alert severity="error" sx={{ mb: 2 }}>
              {caError}
            </Alert>
          )}
          <DialogContentText>
            {caConfirm?.action === "push" ? (
              <>
                This sends <strong>{caConfirm.label}</strong> ({caConfirm.deviceId}) a pointer to
                this relay&apos;s current CA certificate. The pager fetches it, verifies its hash,
                and switches to validating the broker&apos;s certificate the next time it
                reconnects. This does not happen immediately.
              </>
            ) : (
              <>
                This tells <strong>{caConfirm?.label}</strong> ({caConfirm?.deviceId}) to stop
                validating the broker&apos;s certificate. The pager will connect without
                verifying who it is talking to. Use this only to recover a device stuck unable to
                connect; it does not happen immediately.
              </>
            )}
          </DialogContentText>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setCaConfirm(null)} disabled={caBusy}>
            Cancel
          </Button>
          <Button
            onClick={() => void submitCaAction()}
            disabled={caBusy}
            color={caConfirm?.action === "unpin" ? "warning" : "primary"}
          >
            {caConfirm?.action === "push" ? "Push CA" : "Un-pin CA"}
          </Button>
        </DialogActions>
      </Dialog>

      <Snackbar
        open={caSuccess !== null}
        autoHideDuration={5000}
        onClose={() => setCaSuccess(null)}
        message={caSuccess}
      />
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

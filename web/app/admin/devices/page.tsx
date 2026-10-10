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
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import DeviceCard, { type DeviceRow, type PendingCfgDoc, type ProvisionState } from "@/components/DeviceCard";
import FirmwareUpdateDialog from "@/components/FirmwareUpdateDialog";
import RequireAuth from "@/components/RequireAuth";
import SoracomSimsSection from "@/components/SoracomSimsSection";
import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import { useNewestBuild } from "@/lib/firmware";
import type { DeviceDoc, UserDoc } from "@/lib/types";

import SetupCodePanel, { type SetupCodeResult } from "./SetupCodePanel";
import { DEVICE_LABEL_MAX_CHARS } from "@/lib/devices";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

// docs/V02_DESIGN.md §4.4: `POST /api/admin/devices/{id}/ca`
// `{"action":"push"|"unpin"}` (`relay/app/routers/admin.py`'s `push_ca`).
type CaAction = "push" | "unpin";

interface CaConfirmState {
  deviceId: string;
  label: string;
  action: CaAction;
}

const ALL_FAMILIES = "__all__";

const emptyForm = { ownerAlias: "", label: "", defaultToAlias: "", familyId: "" };

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
  const fullScreen = useFullScreenDialog();
  const { uidToAlias } = useDirectory();
  const { familyId: scopeFamilyId, families } = useFamily();
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [users, setUsers] = useState<{ uid: string; alias: string; familyId: string | null }[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);
  const [setupResult, setSetupResult] = useState<SetupCodeResult | null>(null);
  const [caConfirm, setCaConfirm] = useState<CaConfirmState | null>(null);
  const [fwDeviceId, setFwDeviceId] = useState<string | null>(null);
  const newestBuild = useNewestBuild(true, "admin");
  const [caBusy, setCaBusy] = useState(false);
  const [caError, setCaError] = useState<string | null>(null);
  const [caSuccess, setCaSuccess] = useState<string | null>(null);
  // "All" by default only when the switcher itself has no family selected
  // (docs/FAMILIES_TASKS.md 1.8: "default = the switcher family, 'All'
  // option").
  const [filterFamily, setFilterFamily] = useState<string>(scopeFamilyId ?? ALL_FAMILIES);

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
      const rows: { uid: string; alias: string; familyId: string | null }[] = [];
      snap.forEach((d) => {
        const data = d.data() as UserDoc;
        rows.push({ uid: d.id, alias: data.alias, familyId: data.familyId ?? null });
      });
      setUsers(rows);
    });
    return () => {
      unsubDevices();
      unsubUsers();
    };
  }, []);

  function openCreate() {
    // Defaults the dialog's family picker to the switcher's family, without
    // clobbering a family the admin already picked earlier in this session.
    setForm((f) => ({ ...f, familyId: f.familyId || scopeFamilyId || "" }));
    setCreateOpen(true);
  }

  const familyNameById = new Map(families.map((f) => [f.id, f.name]));
  const visibleDevices =
    filterFamily === ALL_FAMILIES ? devices : devices.filter((d) => d.familyId === filterFamily);
  const ownerOptions = form.familyId ? users.filter((u) => u.familyId === form.familyId) : users;

  async function createDevice() {
    setError(null);
    try {
      const resp = await api.post<DeviceSetupResponse>("/admin/devices", {
        ownerAlias: form.ownerAlias,
        label: form.label,
        defaultToAlias: form.defaultToAlias || null,
      });
      setCreateOpen(false);
      setForm({ ...emptyForm, familyId: scopeFamilyId ?? "" });
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

  const formValid = form.ownerAlias.trim() && form.label.trim();
  const setupResultDevice = setupResult
    ? devices.find((d) => d.id === setupResult.deviceId)
    : undefined;
  const setupResultOnline = setupResultDevice?.provisionState === "provisioned";

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ flexWrap: "wrap", justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Devices</Typography>
        <Button variant="contained" onClick={openCreate}>
          Add device
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

      <Stack spacing={2}>
        {visibleDevices.map((d) => (
          <DeviceCard
            key={d.id}
            device={d}
            scope="admin"
            newestBuild={newestBuild}
            ownerLabel={`@${uidToAlias(d.ownerUid) ?? d.ownerUid.slice(0, 8)}`}
            defaultToLabel={d.defaultToUid ? `@${uidToAlias(d.defaultToUid) ?? d.defaultToUid.slice(0, 8)}` : "--"}
            familyLabel={d.familyId ? (familyNameById.get(d.familyId) ?? d.familyId) : "--"}
            onRotate={() => void rotate(d)}
            onRevoke={() => void revoke(d.id)}
            onDelete={() => void deleteDevice(d.id)}
            onFirmware={() => setFwDeviceId(d.id)}
            onCa={(action) => setCaConfirm({ deviceId: d.id, label: d.label, action })}
            onSetAutoLock={(m) => void setAutoLock(d.id, m)}
            onClearPasscode={() => void clearPasscode(d.id)}
          />
        ))}
      </Stack>

      <SoracomSimsSection />

      <Dialog fullScreen={fullScreen} open={createOpen} onClose={() => setCreateOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Add device</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            <TextField
              label="Label"
              value={form.label}
              onChange={(e) => setForm({ ...form, label: e.target.value })}
              helperText="Shown on the web and on the pager's device screen"
              slotProps={{ htmlInput: { maxLength: DEVICE_LABEL_MAX_CHARS } }}
              autoFocus
              fullWidth
            />
            <TextField
              select
              label="Family"
              value={form.familyId}
              onChange={(e) => setForm({ ...form, familyId: e.target.value, ownerAlias: "" })}
              helperText="Narrows the owner picker below; the device's own family is set from its owner."
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
              label="Owner"
              value={form.ownerAlias}
              onChange={(e) => setForm({ ...form, ownerAlias: e.target.value })}
              fullWidth
            >
              {ownerOptions.map((u) => (
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

      <Dialog fullScreen={fullScreen} open={caConfirm !== null} onClose={() => (caBusy ? undefined : setCaConfirm(null))}>
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

      {fwDeviceId !== null && (
        <FirmwareUpdateDialog
          device={devices.find((x) => x.id === fwDeviceId) ?? null}
          scope="admin"
          open
          onClose={() => setFwDeviceId(null)}
        />
      )}

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
    <RequireAuth requireRole="super">
      <AppShell>
        <DevicesInner />
      </AppShell>
    </RequireAuth>
  );
}

"use client";

/** `/family/devices` -- docs/FAMILIES_DESIGN.md §5.4 Devices,
 * docs/FAMILIES_TASKS.md 1.9: a family-admin-scoped copy of `/admin/devices`
 * (`web/app/admin/devices/page.tsx`) -- same table and dialogs, but reading
 * `devices where familyId == useFamily().familyId` instead of the whole
 * collection, the owner/default pickers built from the directory's family
 * members instead of a separate `users` listener, and every write rerouted
 * to `/api/family/devices*` (docs/FAMILIES_DESIGN.md §4) with `familyQuery()`
 * appended for a super browsing a chosen family.
 *
 * See `web/app/admin/devices/page.tsx`'s docstring for the setup-code,
 * lock-control and CA-trust design notes -- unchanged here, just re-scoped.
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import { useEffect, useMemo, useState } from "react";
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
import { ApiError, api } from "@/lib/api";
import { useNewestBuild } from "@/lib/firmware";
import { useDirectory } from "@/lib/directory";
import { familyQuery, useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import type { DeviceDoc } from "@/lib/types";

import BridgePhonesSection from "@/components/BridgePhonesSection";
import SetupCodePanel, { type SetupCodeResult } from "../../admin/devices/SetupCodePanel";
import { DEVICE_LABEL_MAX_CHARS } from "@/lib/devices";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

// docs/V02_DESIGN.md §4.4: `POST /api/family/devices/{id}/ca`
// `{"action":"push"|"unpin"}` (`relay/app/routers/family.py`, same body as
// today's `relay/app/routers/admin.py`'s `push_ca`).
type CaAction = "push" | "unpin";

interface CaConfirmState {
  deviceId: string;
  label: string;
  action: CaAction;
}

const emptyForm = { ownerAlias: "", label: "", defaultToAlias: "" };

// `relay/app/routers/family.py`'s device-create/rotate response -- same
// shape as `relay/app/routers/admin.py`'s `DeviceSetupCodeResponse` (S2.2).
interface DeviceSetupResponse {
  device: { id: string };
  setupCode: string;
  expiresAt: string;
  brokerPush: "pushed" | "manual";
  manualAcl?: string[] | null;
}

function FamilyDevicesInner() {
  const fullScreen = useFullScreenDialog();
  const { byUid, contacts } = useDirectory();
  const { familyId } = useFamily();
  const [fwDeviceId, setFwDeviceId] = useState<string | null>(null);
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  // The relay requires a device on the family build list; the build list is
  // the same for every device of the family, so use the first one.
  const newestBuild = useNewestBuild(true, "family", devices[0]?.id);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState(emptyForm);
  const [setupResult, setSetupResult] = useState<SetupCodeResult | null>(null);
  const [caConfirm, setCaConfirm] = useState<CaConfirmState | null>(null);
  const [caBusy, setCaBusy] = useState(false);
  const [caError, setCaError] = useState<string | null>(null);
  const [caSuccess, setCaSuccess] = useState<string | null>(null);

  // Owner/default pickers: family members only, from the directory rather
  // than a second `users` listener (docs/FAMILIES_TASKS.md 1.9).
  const familyMembers = useMemo(
    () => contacts.filter((c) => byUid(c.uid)?.familyId === familyId),
    [contacts, byUid, familyId]
  );

  useEffect(() => {
    if (!familyId) {
      // No family in scope yet -- leave whatever state already held rather
      // than clearing synchronously in the effect body (same convention as
      // `lib/directory.tsx`'s family listener).
      return;
    }
    const q = query(collection(getFirestoreDb(), "devices"), where("familyId", "==", familyId));
    const unsubscribe = onSnapshot(q, (snap) => {
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
    return unsubscribe;
  }, [familyId]);

  async function createDevice() {
    setError(null);
    try {
      const resp = await api.post<DeviceSetupResponse>(`/family/devices${familyQuery()}`, {
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
        `/family/devices/${device.id}/rotate-credentials${familyQuery()}`
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
      await api.post(`/family/devices/${deviceId}/revoke${familyQuery()}`);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to revoke device");
    }
  }

  async function deleteDevice(deviceId: string) {
    setError(null);
    if (!window.confirm(`Delete device ${deviceId}? This cannot be undone.`)) return;
    try {
      await api.del(`/family/devices/${deviceId}${familyQuery()}`);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to delete device");
    }
  }

  async function setAutoLock(deviceId: string, minutes: number) {
    setError(null);
    try {
      await api.post(`/family/devices/${deviceId}/cfg${familyQuery()}`, { lock: { auto: minutes } });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to set auto-lock");
    }
  }

  async function clearPasscode(deviceId: string) {
    setError(null);
    if (!window.confirm(`Clear ${deviceId}'s passcode? The pager unlocks immediately.`)) return;
    try {
      await api.post(`/family/devices/${deviceId}/cfg${familyQuery()}`, { lock: { clear: true } });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to clear passcode");
    }
  }

  async function submitCaAction() {
    if (!caConfirm) return;
    setCaBusy(true);
    setCaError(null);
    try {
      await api.post(`/family/devices/${caConfirm.deviceId}/ca${familyQuery()}`, {
        action: caConfirm.action,
      });
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
        <Button variant="contained" onClick={() => setCreateOpen(true)}>
          Add device
        </Button>
      </Stack>
      {error && <Alert severity="error">{error}</Alert>}

      <Stack spacing={2}>
        {devices.map((d) => (
          <DeviceCard
            key={d.id}
            device={d}
            scope="family"
            newestBuild={newestBuild}
            ownerLabel={`@${byUid(d.ownerUid)?.alias ?? d.ownerUid.slice(0, 8)}`}
            defaultToLabel={d.defaultToUid ? `@${byUid(d.defaultToUid)?.alias ?? d.defaultToUid.slice(0, 8)}` : "--"}
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
              label="Owner"
              value={form.ownerAlias}
              onChange={(e) => setForm({ ...form, ownerAlias: e.target.value })}
              fullWidth
            >
              {familyMembers.map((u) => (
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
              {familyMembers.map((u) => (
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

      <BridgePhonesSection
        familyId={familyId}
        members={familyMembers
          .filter((c) => byUid(c.uid)?.kind === "person")
          .map((c) => ({ uid: c.uid, alias: c.alias, displayName: c.displayName }))}
      />

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
          scope="family"
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

export default function FamilyDevicesPage() {
  return (
    <RequireAuth requireRole="admin">
      <AppShell>
        <FamilyDevicesInner />
      </AppShell>
    </RequireAuth>
  );
}

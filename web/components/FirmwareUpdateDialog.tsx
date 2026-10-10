import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import FormControl from "@mui/material/FormControl";
import FormControlLabel from "@mui/material/FormControlLabel";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import Typography from "@mui/material/Typography";

import { deviceName } from "@/lib/devices";
import { ApiError } from "@/lib/api";
import {
  MONTHLY_TARGET_BYTES,
  cancelOta,
  formatBytes,
  listBuilds,
  pushOta,
  type FirmwareBuild,
  type FirmwareScope,
} from "@/lib/firmware";
import type { DeviceDoc } from "@/lib/types";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

function errText(e: unknown): string {
  return e instanceof ApiError ? e.message : "Request failed.";
}

/** Mount only while open (state starts fresh each time). "Update firmware" dialog for a super (`admin` scope) or a family admin (`family` scope, own family only) -- docs/OTA_DESIGN.md §1 D10, §4. */
export default function FirmwareUpdateDialog({
  device,
  scope,
  open,
  onClose,
}: {
  device: (DeviceDoc & { id: string }) | null;
  scope: FirmwareScope;
  open: boolean;
  onClose: () => void;
}) {
  const fullScreen = useFullScreenDialog();
  const [builds, setBuilds] = useState<FirmwareBuild[]>([]);
  const [selected, setSelected] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const deviceId = device?.id;

  useEffect(() => {
    if (!open || !deviceId) return;
    let cancelled = false;
    listBuilds(scope, deviceId)
      .then((b) => {
        if (!cancelled) setBuilds([...b].sort((x, y) => y.published - x.published));
      })
      .catch((e) => {
        if (!cancelled) setError(errText(e));
      });
    return () => {
      cancelled = true;
    };
  }, [open, deviceId, scope]);

  // Same normalisation as the relay's 409 check: trim, lowercase, first 16 hex.
  const running = device?.status?.img ? device.status.img.trim().toLowerCase().slice(0, 16) : null;
  const runningBuild = builds.find((b) => b.id16.toLowerCase() === running);
  const capable = device?.status?.otaCap === 1;
  const chosen = builds.find((b) => b.id16 === selected);
  const chosenIsRunning = !!chosen && chosen.id16.toLowerCase() === running;
  const hasJob = !!device?.otaJob;

  async function run(fn: () => Promise<unknown>) {
    setBusy(true);
    setError(null);
    try {
      await fn();
      onClose();
    } catch (e) {
      setError(errText(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <Dialog fullScreen={fullScreen} open={open} onClose={busy ? undefined : onClose} fullWidth maxWidth="sm">
      <DialogTitle>Update firmware{device ? ` -- ${deviceName(device)}` : ""}</DialogTitle>
      <DialogContent>
        <Typography variant="body2" sx={{ mb: 2 }}>
          Running:{" "}
          {runningBuild
            ? `${runningBuild.version} `
            : device?.status?.fw
              ? `${device.status.fw} `
              : ""}
          <code>{running ?? "unknown"}</code>
        </Typography>
        {!capable && (
          <Alert severity="warning" sx={{ mb: 2 }}>
            This pager&apos;s firmware or bootloader cannot take over-the-air updates yet (needs one
            USB flash)
          </Alert>
        )}
        {error && (
          <Alert severity="error" sx={{ mb: 2 }}>
            {error}
          </Alert>
        )}
        {hasJob && (
          <Alert
            severity="info"
            sx={{ mb: 2 }}
            action={
              <Button
                color="inherit"
                size="small"
                disabled={busy || !deviceId}
                onClick={() => deviceId && void run(() => cancelOta(scope, deviceId))}
              >
                Cancel pending update
              </Button>
            }
          >
            An update to {device?.otaJob?.target16} is pending.
          </Alert>
        )}
        <FormControl fullWidth>
          <RadioGroup value={selected} onChange={(e) => setSelected(e.target.value)}>
            {builds.map((b) => {
              const isRunning = running !== null && b.id16.toLowerCase() === running;
              return (
                <FormControlLabel
                  key={b.id16}
                  value={b.id16}
                  disabled={isRunning}
                  control={<Radio />}
                  label={
                    <>
                      <Typography variant="body2">
                        {b.version} <code>{b.id16}</code>
                        {isRunning ? " (running)" : ""}
                      </Typography>
                      <Typography variant="caption" color="text.secondary">
                        {new Date(b.published * 1000).toLocaleDateString()}, {formatBytes(b.size)};{" "}
                        {b.onDemandDelta ? "delta (made at push)" : b.kind}, ~{formatBytes(b.estBytes)}
                      </Typography>
                    </>
                  }
                />
              );
            })}
          </RadioGroup>
        </FormControl>
        {chosen && (
          <Typography variant="body2" sx={{ mt: 2 }}>
            Cellular data for this update:{" "}
            {chosen.onDemandDelta
              ? `up to ~${formatBytes(chosen.estBytes)}; a delta is generated at push time and is usually much smaller`
              : `~${formatBytes(chosen.estBytes)}`}{" "}
            (about{" "}
            {((chosen.estBytes / MONTHLY_TARGET_BYTES) * 100).toFixed(1)} % of the 10 MB monthly
            target)
          </Typography>
        )}
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} disabled={busy}>
          Close
        </Button>
        <Button
          variant="contained"
          disabled={busy || !capable || !chosen || chosenIsRunning || !deviceId}
          onClick={() => deviceId && chosen && void run(() => pushOta(scope, deviceId, chosen.id16))}
        >
          Send update
        </Button>
      </DialogActions>
    </Dialog>
  );
}

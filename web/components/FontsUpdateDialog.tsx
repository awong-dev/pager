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
import { cancelAssets, isAssetJob, listAssets, pushAssets, type AssetBuild } from "@/lib/assets";
import { MONTHLY_TARGET_BYTES, formatBytes, type FirmwareScope } from "@/lib/firmware";
import type { DeviceDoc } from "@/lib/types";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

function errText(e: unknown): string {
  return e instanceof ApiError ? e.message : "Request failed.";
}

/** Mount only while open (state starts fresh each time). "Update fonts" dialog -- docs/ASSETS_OTA_DESIGN.md §3. A 409 from the relay (no font partition, or another job live) is shown verbatim. */
export default function FontsUpdateDialog({
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
  const [builds, setBuilds] = useState<AssetBuild[]>([]);
  const [selected, setSelected] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const deviceId = device?.id;

  useEffect(() => {
    if (!open || !deviceId) return;
    let cancelled = false;
    listAssets(scope, deviceId)
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

  const src = device?.status?.assetsSrc ?? null;
  const active = src === 2 && device?.status?.assetsId ? device.status.assetsId.toLowerCase() : null;
  const capable = src === 1 || src === 2;
  const chosen = builds.find((b) => b.id16 === selected);
  const chosenIsActive = !!chosen && chosen.id16 === active;
  const hasJob = !!device?.otaJob;
  const assetJob = isAssetJob(device);
  const activeBuild = builds.find((b) => b.id16 === active);

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
      <DialogTitle>Update fonts{device ? ` -- ${deviceName(device)}` : ""}</DialogTitle>
      <DialogContent>
        <Typography variant="body2" sx={{ mb: 2 }}>
          Fonts now:{" "}
          {src === 1 ? (
            "factory"
          ) : src === 2 ? (
            <>
              {activeBuild ? `${activeBuild.label} ` : ""}
              <code>{active}</code>
            </>
          ) : (
            "unknown"
          )}
        </Typography>
        {!capable && (
          <Alert severity="warning" sx={{ mb: 2 }}>
            This pager&apos;s firmware or partition table cannot take font updates yet (needs firmware
            with font OTA and one USB table flash)
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
              assetJob ? (
                <Button
                  color="inherit"
                  size="small"
                  disabled={busy || !deviceId}
                  onClick={() => deviceId && void run(() => cancelAssets(scope, deviceId))}
                >
                  Cancel pending update
                </Button>
              ) : undefined
            }
          >
            {assetJob
              ? `A font update to ${device?.otaJob?.target16} is pending.`
              : "A firmware update is pending; fonts can be updated after it finishes or is cancelled."}
          </Alert>
        )}
        <FormControl fullWidth>
          <RadioGroup value={selected} onChange={(e) => setSelected(e.target.value)}>
            {builds.map((b) => {
              const isActive = active !== null && b.id16 === active;
              return (
                <FormControlLabel
                  key={b.id16}
                  value={b.id16}
                  disabled={isActive}
                  control={<Radio />}
                  label={
                    <>
                      <Typography variant="body2">
                        {b.label} <code>{b.id16}</code>
                        {isActive ? " (active)" : ""}
                      </Typography>
                      <Typography variant="caption" color="text.secondary">
                        {new Date(b.published * 1000).toLocaleDateString()}, {formatBytes(b.size)}; full, ~
                        {formatBytes(b.estBytes)}
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
            Cellular data for this update: ~{formatBytes(chosen.estBytes)} (about{" "}
            {((chosen.estBytes / MONTHLY_TARGET_BYTES) * 100).toFixed(1)} % of the 10 MB monthly
            target). The pager shows factory fonts until the download finishes.
          </Typography>
        )}
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} disabled={busy}>
          Close
        </Button>
        <Button
          variant="contained"
          disabled={busy || !capable || !chosen || chosenIsActive || !deviceId}
          onClick={() => deviceId && chosen && void run(() => pushAssets(scope, deviceId, chosen.id16))}
        >
          Send fonts
        </Button>
      </DialogActions>
    </Dialog>
  );
}

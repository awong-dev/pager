"use client";

import { useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import TextField from "@mui/material/TextField";
import EditIcon from "@mui/icons-material/Edit";

import { ApiError } from "@/lib/api";
import { DEVICE_LABEL_MAX_CHARS, deviceName, patchDeviceLabel } from "@/lib/devices";
import type { FirmwareScope } from "@/lib/firmware";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

interface DialogProps {
  device: { id: string; label: string };
  scope: FirmwareScope;
  onClose: () => void;
}

function LabelDialog({ device, scope, onClose }: DialogProps) {
  const fullScreen = useFullScreenDialog();
  const [value, setValue] = useState(device.label);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save() {
    setBusy(true);
    setError(null);
    try {
      await patchDeviceLabel(scope, device.id, value.trim());
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to rename device");
      setBusy(false);
    }
  }

  return (
    <Dialog fullScreen={fullScreen} open onClose={busy ? undefined : onClose} fullWidth maxWidth="xs">
      <DialogTitle>Rename {deviceName(device)}</DialogTitle>
      <DialogContent>
        {error && (
          <Alert severity="error" sx={{ mb: 2 }}>
            {error}
          </Alert>
        )}
        <TextField
          autoFocus
          fullWidth
          label="Label"
          value={value}
          onChange={(e) => setValue(e.target.value)}
          slotProps={{ htmlInput: { maxLength: DEVICE_LABEL_MAX_CHARS } }}
          helperText="The pager shows the new name after its next credential rotation."
          sx={{ mt: 1 }}
        />
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} disabled={busy}>
          Cancel
        </Button>
        <Button onClick={() => void save()} disabled={busy || !value.trim()}>
          Save
        </Button>
      </DialogActions>
    </Dialog>
  );
}

/** Pencil button that opens the rename dialog for one device. */
export default function EditDeviceLabelButton({ device, scope }: { device: { id: string; label: string }; scope: FirmwareScope }) {
  const [open, setOpen] = useState(false);
  return (
    <>
      <IconButton size="small" aria-label={`rename ${deviceName(device)}`} onClick={() => setOpen(true)}>
        <EditIcon fontSize="inherit" />
      </IconButton>
      {open && <LabelDialog device={device} scope={scope} onClose={() => setOpen(false)} />}
    </>
  );
}

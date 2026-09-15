"use client";

/** Shared panel for both `POST /admin/devices` and
 * `POST /admin/devices/{id}/rotate-credentials` (docs/DEVICE_TASKS.md
 * W2.3, S2.2's `DeviceSetupCodeResponse`): shows the one-time setup code
 * big, a copy button, a QR code, the two-step typed-code instructions from
 * `docs/DEVICE_PLAN.md` §3.2, and an `expiresAt` countdown. `manualAcl` is
 * shown only when `brokerPush === "manual"` (`docs/DEVICE_PLAN.md` §3.2's
 * "deployments where the relay cannot manage broker users").
 *
 * The device's own `provisionState` (passed in as `online`, derived by the
 * caller from its existing `devices` Firestore listener --
 * `docs/DEVICE_PLAN.md` §3.2: "provisionState becomes provisioned when the
 * first signed /status ... arrives, which is what flips the Add device page
 * to 'pgr-0001 is online'") is shown live while this panel is open.
 */

import { useEffect, useRef, useState } from "react";
import QRCode from "qrcode";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Divider from "@mui/material/Divider";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

export interface SetupCodeResult {
  deviceId: string;
  label: string;
  setupCode: string;
  expiresAt: string; // ISO timestamp
  brokerPush: "pushed" | "manual";
  manualAcl?: string[] | null;
}

function formatCountdown(msRemaining: number): string {
  if (msRemaining <= 0) return "expired";
  const totalS = Math.floor(msRemaining / 1000);
  const m = Math.floor(totalS / 60);
  const s = totalS % 60;
  return `${m}:${s.toString().padStart(2, "0")}`;
}

export default function SetupCodePanel({
  result,
  online,
  onClose,
}: {
  result: SetupCodeResult;
  online: boolean;
  onClose: () => void;
}) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [copied, setCopied] = useState(false);
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    const interval = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(interval);
  }, []);

  useEffect(() => {
    if (canvasRef.current) {
      // Fire-and-forget: a QR render failure (e.g. an over-length string)
      // should not block the code from being read as plain text.
      void QRCode.toCanvas(canvasRef.current, result.setupCode, { width: 220 });
    }
  }, [result.setupCode]);

  const expiresMs = new Date(result.expiresAt).getTime();
  const msRemaining = expiresMs - now;
  const expired = msRemaining <= 0;

  async function copyCode() {
    await navigator.clipboard.writeText(result.setupCode);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  }

  return (
    <Dialog open onClose={onClose} fullWidth maxWidth="xs">
      <DialogTitle>Setup code for {result.label}</DialogTitle>
      <DialogContent>
        <Stack spacing={2}>
          <Alert severity={online ? "success" : "info"}>
            {online ? `${result.deviceId} is online` : "Waiting for the device to connect..."}
          </Alert>

          <Typography
            variant="h5"
            sx={{ fontFamily: "monospace", wordBreak: "break-all", textAlign: "center" }}
          >
            {result.setupCode}
          </Typography>

          <Stack direction="row" spacing={2} sx={{ justifyContent: "center", alignItems: "center" }}>
            <Button variant="outlined" onClick={() => void copyCode()} disabled={expired}>
              {copied ? "Copied" : "Copy code"}
            </Button>
            <Chip
              label={expired ? "expired" : `expires in ${formatCountdown(msRemaining)}`}
              color={expired ? "error" : "default"}
              size="small"
            />
          </Stack>

          <Box sx={{ display: "flex", justifyContent: "center" }}>
            <canvas ref={canvasRef} />
          </Box>

          <Divider />

          <Typography variant="subtitle2">On the pager (docs/DEVICE_PLAN.md §3.2)</Typography>
          <Typography variant="body2">
            1. Power on a brand-new device (it enters Setup mode automatically), or from the
            Device screen choose &ldquo;Set up again&rdquo; (or hold the button for 10 s during
            boot).
          </Typography>
          <Typography variant="body2">
            2. When the pager asks &ldquo;Type the setup code from your pager website&rdquo;,
            enter the code above -- hyphens and spaces are optional, and each four-character group
            is checked as you type.
          </Typography>

          {result.brokerPush === "manual" && result.manualAcl && result.manualAcl.length > 0 && (
            <>
              <Divider />
              <Alert severity="warning">
                The relay could not push this device&apos;s broker credential automatically. Enter
                these ACL rules on the broker by hand:
              </Alert>
              <Box
                component="pre"
                sx={{
                  m: 0,
                  p: 1,
                  bgcolor: "action.hover",
                  borderRadius: 1,
                  fontSize: "0.8rem",
                  overflowX: "auto",
                }}
              >
                {result.manualAcl.join("\n")}
              </Box>
            </>
          )}

          {expired && (
            <Alert severity="error">
              This code has expired. Use Rotate to issue a new one.
            </Alert>
          )}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>Done</Button>
      </DialogActions>
    </Dialog>
  );
}

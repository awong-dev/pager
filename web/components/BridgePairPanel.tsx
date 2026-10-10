"use client";

/** Pairing-code dialog for a bridge phone -- docs/BRIDGE_PHONE_DESIGN.md
 * decisions 2 and 13: the 8-digit code (10-minute expiry) and the phone setup
 * checklist. The bridge row is passed in live (the section polls the list), so
 * the dialog flips to "paired" and shows the SIM / Voice numbers the phone
 * reported without a refresh. Modelled on `admin/devices/SetupCodePanel`.
 */

import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Divider from "@mui/material/Divider";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import { toMs } from "@/lib/bridges";
import type { ApiTime, BridgeRow } from "@/lib/types";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

// docs/BRIDGE_PHONE_DESIGN.md decision 13 "Phone setup".
const CHECKLIST = [
  "Sign in to the member's own Google account (Chat and Voice): notifications on, contacts list empty.",
  "Open the Bridge app and tap Grant runtime permissions.",
  "Enable Notification access.",
  "Make the Bridge app the Default SMS app (SIM phones only).",
  "Exempt the app from battery optimisation.",
  "Enable the Accessibility service.",
  "Fill in the relay URL (above) and the numbers, then tap Save fields.",
  "Enter the code above and tap Pair.",
  "Set the screen lock to None and turn Do Not Disturb off.",
  "Never leave Chat open on a thread (an open thread suppresses its notifications); use a charge limiter or a smart-plug duty cycle so the battery lasts.",
];

function countdown(ms: number): string {
  if (ms <= 0) return "expired";
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${(s % 60).toString().padStart(2, "0")}`;
}

export default function BridgePairPanel({
  bridge,
  memberLabel,
  code,
  expiresAt,
  onEditNumbers,
  onClose,
}: {
  bridge: BridgeRow;
  memberLabel: string;
  code: string;
  expiresAt: ApiTime;
  onEditNumbers: () => void;
  onClose: () => void;
}) {
  const fullScreen = useFullScreenDialog();
  const [now, setNow] = useState(() => Date.now());
  const [copied, setCopied] = useState(false);
  // The dialog only mounts after a click, but guard for the static export pass.
  const [origin] = useState(() => (typeof window === "undefined" ? "" : window.location.origin));
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);

  const exp = toMs(expiresAt);
  const remaining = exp === null ? 0 : exp - now;
  const paired = bridge.paired;
  const sim = bridge.simNumber ?? bridge.status.simNumber ?? null;

  async function copy() {
    await navigator.clipboard.writeText(code);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  }

  return (
    <Dialog fullScreen={fullScreen} open onClose={onClose} fullWidth maxWidth="xs">
      <DialogTitle>Pair {bridge.label}</DialogTitle>
      <DialogContent>
        <Stack spacing={2}>
          <Alert severity={paired ? "success" : "info"}>
            {paired ? `${bridge.label} is paired for ${memberLabel}` : "Waiting for the phone to pair..."}
          </Alert>

          <Typography variant="h4" sx={{ fontFamily: "monospace", textAlign: "center", letterSpacing: 4 }}>
            {code}
          </Typography>
          <Stack direction="row" spacing={2} useFlexGap sx={{ flexWrap: "wrap", justifyContent: "center", alignItems: "center" }}>
            <Button variant="outlined" onClick={() => void copy()} disabled={remaining <= 0}>
              {copied ? "Copied" : "Copy code"}
            </Button>
            <Chip
              size="small"
              color={remaining <= 0 ? "error" : "default"}
              label={remaining <= 0 ? "expired" : `expires in ${countdown(remaining)}`}
            />
          </Stack>

          <Divider />

          <Stack direction="row" spacing={1} sx={{ alignItems: "center", flexWrap: "wrap" }} useFlexGap>
            <Chip size="small" variant="outlined" label={`SIM: ${sim ?? "none"}`} />
            <Chip size="small" variant="outlined" label={`Voice: ${bridge.voiceNumber ?? bridge.status.voiceNumber ?? "none"}`} />
            {!sim && <Chip size="small" color="info" label="Voice only" />}
            <Button size="small" onClick={onEditNumbers}>
              Edit numbers
            </Button>
          </Stack>
          <Typography variant="caption" color="text.secondary">
            The phone cannot read its Google Voice number: enter it on the phone&apos;s setup screen or here.
          </Typography>

          <Divider />
          <Typography variant="subtitle2">Phone setup</Typography>
          <Typography variant="body2">
            Relay URL: <code>{origin}</code> (the app&apos;s default already points here). Code expires in 10 minutes.
          </Typography>
          <Stack component="ol" spacing={0.5} sx={{ m: 0, pl: 3 }}>
            {CHECKLIST.map((c) => (
              <Typography key={c} component="li" variant="body2">
                {c}
              </Typography>
            ))}
          </Stack>
          {remaining <= 0 && !paired && (
            <Alert severity="error">This code has expired. Close this and use New code.</Alert>
          )}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>Done</Button>
      </DialogActions>
    </Dialog>
  );
}

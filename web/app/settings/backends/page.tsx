"use client";

/** `/settings/backends` -- docs/SERVER_PLAN.md §7.2: list + add backends
 * (SMS phone verify, Google Chat link code) + enable toggles. Reads the
 * list from `users/{uid}/backends` directly (Firestore, always allowed for
 * one's own uid); every write goes through `/api/me/backends*`
 * (`relay/app/routers/me.py`).
 *
 * An `sms` or `gchat` backend is created disabled and unverified; creating
 * it triggers the adapter's `start_link()` (an SMS with a code, or a Google
 * Chat link code), and the Verify dialog below posts the code back to
 * `POST /api/me/backends/{id}/verify`, which enables the row on success.
 */

import {
  collection,
  onSnapshot,
} from "firebase/firestore";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import DeleteIcon from "@mui/icons-material/Delete";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { getFirestoreDb } from "@/lib/firebase";
import type { BackendDoc } from "@/lib/types";

interface BackendRow extends BackendDoc {
  id: string;
}

function configSummary(b: BackendRow): string {
  if (b.kind === "sms") return typeof b.config.phone === "string" ? b.config.phone : "(no phone)";
  if (b.kind === "gchat") return typeof b.config.space === "string" ? b.config.space : "not linked";
  if (b.kind === "pager") return typeof b.config.deviceId === "string" ? b.config.deviceId : "";
  return "";
}

function BackendsInner() {
  const { me } = useAuth();
  const [backends, setBackends] = useState<BackendRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [addSmsOpen, setAddSmsOpen] = useState(false);
  const [phone, setPhone] = useState("");
  const [verifyBackendId, setVerifyBackendId] = useState<string | null>(null);
  const [code, setCode] = useState("");
  const [verifyError, setVerifyError] = useState<string | null>(null);

  useEffect(() => {
    if (!me) return;
    const db = getFirestoreDb();
    const unsubscribe = onSnapshot(collection(db, "users", me.uid, "backends"), (snap) => {
      const rows: BackendRow[] = [];
      snap.forEach((d) => rows.push({ id: d.id, ...(d.data() as BackendDoc) }));
      setBackends(rows);
    });
    return unsubscribe;
  }, [me]);

  async function toggleEnabled(b: BackendRow) {
    setError(null);
    try {
      await api.patch(`/me/backends/${b.id}`, { enabled: !b.enabled });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to update");
    }
  }

  async function removeBackend(b: BackendRow) {
    setError(null);
    try {
      await api.del(`/me/backends/${b.id}`);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to remove");
    }
  }

  async function addSms() {
    setError(null);
    try {
      const created = await api.post<BackendRow>("/me/backends", {
        kind: "sms",
        config: { phone },
        enabled: true,
      });
      setAddSmsOpen(false);
      setPhone("");
      setVerifyBackendId(created.id);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to add SMS backend");
    }
  }

  async function addGchat() {
    setError(null);
    try {
      await api.post<BackendRow>("/me/backends", { kind: "gchat", config: {}, enabled: true });
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to add Google Chat backend");
    }
  }

  async function submitVerify() {
    if (!verifyBackendId) return;
    setVerifyError(null);
    try {
      await api.post(`/me/backends/${verifyBackendId}/verify`, { code });
      setVerifyBackendId(null);
      setCode("");
    } catch (err) {
      setVerifyError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Verification failed"
      );
    }
  }

  return (
    <Stack spacing={2}>
      <Typography variant="h5">Backends</Typography>
      {error && <Alert severity="error">{error}</Alert>}

      <Stack spacing={1.5}>
        {backends.map((b) => (
          <Card key={b.id} variant="outlined">
            <CardContent>
              <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
                <Chip label={b.kind} size="small" />
                <Typography sx={{ flexGrow: 1 }}>{configSummary(b)}</Typography>
                {!b.verifiedAt && b.kind !== "webapp" && b.kind !== "pager" && (
                  <Chip label="unverified" color="warning" size="small" />
                )}
                <Switch checked={b.enabled} onChange={() => void toggleEnabled(b)} />
                {b.kind !== "pager" && b.kind !== "webapp" && (
                  <IconButton onClick={() => void removeBackend(b)} aria-label="remove">
                    <DeleteIcon fontSize="small" />
                  </IconButton>
                )}
              </Stack>
            </CardContent>
          </Card>
        ))}
      </Stack>

      <Stack direction="row" spacing={2}>
        <Button variant="outlined" onClick={() => setAddSmsOpen(true)}>
          Add SMS
        </Button>
        <Button variant="outlined" onClick={() => void addGchat()}>
          Add Google Chat
        </Button>
      </Stack>

      <Dialog open={addSmsOpen} onClose={() => setAddSmsOpen(false)}>
        <DialogTitle>Add SMS backend</DialogTitle>
        <DialogContent>
          <TextField
            autoFocus
            margin="dense"
            label="Phone (+1XXXXXXXXXX)"
            fullWidth
            value={phone}
            onChange={(e) => setPhone(e.target.value)}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setAddSmsOpen(false)}>Cancel</Button>
          <Button onClick={() => void addSms()} disabled={!phone.trim()}>
            Add
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={verifyBackendId !== null} onClose={() => setVerifyBackendId(null)}>
        <DialogTitle>Verify SMS number</DialogTitle>
        <DialogContent>
          <Typography variant="body2" sx={{ mb: 2 }}>
            Enter the code sent to your phone.
          </Typography>
          {verifyError && <Alert severity="warning" sx={{ mb: 2 }}>{verifyError}</Alert>}
          <TextField
            autoFocus
            margin="dense"
            label="Code"
            fullWidth
            value={code}
            onChange={(e) => setCode(e.target.value)}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setVerifyBackendId(null)}>Close</Button>
          <Button onClick={() => void submitVerify()} disabled={!code.trim()}>
            Verify
          </Button>
        </DialogActions>
      </Dialog>

      <Box>
        <Alert severity="info">
          SMS and Google Chat backends stay disabled until you verify them. Adding one sends a
          code; enter it with Verify to enable delivery.
        </Alert>
      </Box>
    </Stack>
  );
}

export default function BackendsPage() {
  return (
    <RequireAuth>
      <AppShell>
        <BackendsInner />
      </AppShell>
    </RequireAuth>
  );
}

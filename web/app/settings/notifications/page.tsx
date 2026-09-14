"use client";

/** `/settings/notifications` -- docs/SERVER_PLAN.md §7.2/§7.6: enable
 * browser notifications -> registers an FCM token via
 * `POST /api/me/push-tokens`; a test button. Permission is asked only from
 * here, never on first load.
 */

import { useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import {
  notificationPermission,
  notificationsSupported,
  registerForPush,
  showLocalTestNotification,
} from "@/lib/notifications";

function NotificationsInner() {
  // Lazy initializer (client-only re-evaluated at hydration) rather than a
  // mount effect + setState.
  const [permission, setPermission] = useState<NotificationPermission | "unsupported">(() =>
    notificationPermission()
  );
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  async function handleEnable() {
    setBusy(true);
    setError(null);
    setStatus(null);
    try {
      const token = await registerForPush();
      setPermission(notificationPermission());
      if (!token) {
        setError(
          "Notifications were not enabled -- permission was denied, or this browser doesn't " +
            "support push (see the iOS note below)."
        );
        return;
      }
      await api.post("/me/push-tokens", { token });
      setStatus("Notifications enabled for this browser.");
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to enable notifications");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Stack spacing={2} sx={{ maxWidth: 480 }}>
      <Typography variant="h5">Notifications</Typography>

      {!notificationsSupported() && (
        <Alert severity="warning">This browser does not support the Notifications API.</Alert>
      )}

      <Typography variant="body2" color="text.secondary">
        Current permission: <strong>{permission}</strong>
      </Typography>

      {status && <Alert severity="success">{status}</Alert>}
      {error && <Alert severity="error">{error}</Alert>}

      <Stack direction="row" spacing={2}>
        <Button variant="contained" disabled={busy || !notificationsSupported()} onClick={() => void handleEnable()}>
          Enable notifications
        </Button>
        <Button
          variant="outlined"
          disabled={permission !== "granted"}
          onClick={() => showLocalTestNotification()}
        >
          Send test notification
        </Button>
      </Stack>

      <Alert severity="info">
        The test button shows a notification locally in this browser -- it does not round-trip
        through the relay (there is no &quot;send me a push&quot; API endpoint), so it only checks that
        permission and display work, not the FCM delivery path end to end.
      </Alert>

      <Alert severity="info">
        iOS Safari: Apple only delivers push notifications to a PWA that has been added to the
        Home Screen (Share &rarr; Add to Home Screen) and opened from there at least once. Push
        will not work in a normal Safari tab.
      </Alert>
    </Stack>
  );
}

export default function NotificationsPage() {
  return (
    <RequireAuth>
      <AppShell>
        <NotificationsInner />
      </AppShell>
    </RequireAuth>
  );
}

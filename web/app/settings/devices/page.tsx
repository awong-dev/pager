"use client";

/** `/settings/devices` -- an owner's own pagers, linking into
 * `/devices/[id]` for the SMS contacts editor and audit log
 * (docs/V02_DESIGN.md §6). Admins reach the same detail page from
 * `/admin/devices` instead (that list already exists); this page is for a
 * non-admin parent, who has no `/admin/*` access.
 *
 * `GET /api/devices` (the caller's own devices: id, label, status) was
 * being added by another agent in parallel on this branch for exactly this
 * use -- no such listing existed before. If it 404s (not deployed yet on
 * whatever relay this build is pointed at), this page degrades to a plain
 * explanation rather than a crash; an admin can still always reach a
 * device's page from `/admin/devices`.
 */

import Link from "next/link";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import CircularProgress from "@mui/material/CircularProgress";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import DeviceTrustChip from "@/components/DeviceTrustChip";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import type { DeviceListItem } from "@/lib/smsContacts";

function DevicesInner() {
  const [devices, setDevices] = useState<DeviceListItem[] | null>(null);
  const [unavailable, setUnavailable] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const resp = await api.get<DeviceListItem[]>("/devices");
        if (!cancelled) setDevices(resp);
      } catch (err) {
        if (cancelled) return;
        if (err instanceof ApiError && err.status === 404) {
          setUnavailable(true);
        } else {
          setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load your devices.");
        }
      }
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  return (
    <Stack spacing={2}>
      <Typography variant="h5">My devices</Typography>

      {unavailable && (
        <Alert severity="info">
          This relay doesn&apos;t support listing your own devices yet. Ask an admin to open your
          pager from Admin → Devices instead.
        </Alert>
      )}
      {error && <Alert severity="error">{error}</Alert>}
      {devices === null && !unavailable && !error && <CircularProgress size={24} />}
      {devices !== null && devices.length === 0 && (
        <Typography variant="body2" color="text.secondary">
          No pagers are registered to you yet.
        </Typography>
      )}

      <Stack spacing={1.5}>
        {devices?.map((d) => (
          <Card key={d.id} variant="outlined">
            <CardContent>
              <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
                <Stack sx={{ flexGrow: 1 }}>
                  <Typography variant="subtitle1">{d.label}</Typography>
                  <Typography variant="caption" color="text.secondary">
                    {d.id} -- {d.status?.state ?? "unknown"}
                  </Typography>
                </Stack>
                <DeviceTrustChip tls={d.status?.tls} caFp={d.status?.caFp} />
                <Button size="small" variant="outlined" component={Link} href={`/devices/${d.id}`}>
                  SMS contacts &amp; log
                </Button>
              </Stack>
            </CardContent>
          </Card>
        ))}
      </Stack>
    </Stack>
  );
}

export default function SettingsDevicesPage() {
  return (
    <RequireAuth>
      <AppShell>
        <DevicesInner />
      </AppShell>
    </RequireAuth>
  );
}

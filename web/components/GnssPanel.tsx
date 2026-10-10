"use client";

/** GNSS on/off panel on `/devices/[id]` -- docs/GNSS_DISABLE_DESIGN.md D6.
 * One switch, saved immediately; mirrors `WifiPanel.tsx`'s load/error shape. */

import { useCallback, useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import FormControlLabel from "@mui/material/FormControlLabel";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import Typography from "@mui/material/Typography";

import { ApiError, api } from "@/lib/api";
import { reportedLabel, type GnssConfigResponse, type GnssPutRequest } from "@/lib/gnss";

export default function GnssPanel({ deviceId }: { deviceId: string }) {
  const [config, setConfig] = useState<GnssConfigResponse | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const fetchConfig = useCallback(async () => {
    if (!deviceId || deviceId === "_") return;
    try {
      const resp = await api.get<GnssConfigResponse>(`/devices/${deviceId}/gnss`);
      setConfig(resp);
      setLoadError(null);
    } catch (err) {
      setLoadError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load GPS settings."
      );
    }
  }, [deviceId]);

  useEffect(() => {
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void fetchConfig();
  }, [fetchConfig]);

  async function toggle(en: boolean) {
    setSaving(true);
    setSaveError(null);
    try {
      const body: GnssPutRequest = { en };
      setConfig(await api.put<GnssConfigResponse>(`/devices/${deviceId}/gnss`, body));
    } catch (err) {
      setSaveError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save GPS settings."
      );
    } finally {
      setSaving(false);
    }
  }

  return (
    <Card variant="outlined">
      <CardContent>
        <Typography variant="h6" gutterBottom>
          GPS
        </Typography>
        {loadError && (
          <Alert severity="warning" sx={{ mb: 2 }}>
            {loadError}
          </Alert>
        )}
        {saveError && (
          <Alert severity="error" sx={{ mb: 2 }}>
            {saveError}
          </Alert>
        )}
        {config === null ? (
          !loadError && <CircularProgress size={24} />
        ) : (
          <Stack spacing={1}>
            <Stack direction="row" spacing={1} useFlexGap sx={{ flexWrap: "wrap", alignItems: "center" }}>
              <FormControlLabel
                control={
                  <Switch
                    checked={config.en}
                    disabled={saving}
                    onChange={(e) => void toggle(e.target.checked)}
                  />
                }
                label="GPS (GNSS)"
              />
              {config.pending && (
                <Chip size="small" color="warning" label="waiting for the pager to confirm" />
              )}
            </Stack>
            <Typography variant="body2" color="text.secondary">
              Off: location comes from the cell network only. Use when the GPS antenna is damaged or
              unwanted.
            </Typography>
            <Typography variant="caption" color="text.secondary">
              Device reports: {reportedLabel(config.reported)}
            </Typography>
          </Stack>
        )}
      </CardContent>
    </Card>
  );
}

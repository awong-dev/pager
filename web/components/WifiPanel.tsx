"use client";

/** The WiFi panel on `/devices/[id]` -- docs/WIFI_DESIGN.md §4,
 * `docs/WIFI_TASKS.md` W8. An on/off toggle plus up to two SSID/PSK rows,
 * Save and Clear, following `DevicePageClient.tsx`'s SMS-contacts editor
 * shape (one-shot GET on mount, PUT on Save, relay validation errors shown
 * verbatim).
 *
 * Relay contract this was built against (W7, the relay side, was being
 * built in parallel on this branch -- treat the shapes below as the
 * contract to reconcile against, not a confirmed observation; see
 * `lib/wifi.ts` for the TypeScript mirror):
 *
 *   GET /api/devices/{id}/wifi
 *     -> { en: boolean, nets: [{s: string, set: true}, ...], pending?: boolean }
 *     `nets` has 0-2 entries, in NVS slot order; a GET never carries a PSK.
 *
 *   PUT /api/devices/{id}/wifi   body: { en: boolean, nets?: [{s, p}, ...] }
 *     -> same shape as GET
 *     - `nets` omitted entirely -> "leave the stored networks alone, apply
 *       `en` only" (docs/WIFI_DESIGN.md §4). This is what lets the toggle
 *       save without ever re-sending a password.
 *     - `nets: []` -> clears both stored networks (the Clear button).
 *     - `nets: [{s,p}, ...]` -> wholesale replace, <=2 entries, SSID 1-32
 *       bytes, PSK 8-63 bytes -- 422 on a bad entry.
 *     - 409 (with a `detail` string) when the PUT carries `nets` and the
 *       device's last `/status` didn't report `tls: "pinned"`; `en` alone
 *       is always allowed regardless of TLS state.
 *
 * UX consequence of "a PSK is never displayed back" (spelled out here
 * because it shapes this component, not just documented in `lib/wifi.ts`):
 * since `nets` is a wholesale replace and the server never returns a stored
 * PSK, editing *any* row (including just removing the other one) requires
 * re-typing a password for *every* non-empty row being sent, even a row the
 * user didn't touch. Toggling `en` alone avoids this entirely by omitting
 * `nets` from the request -- tracked here as `netsTouched`.
 */

import { useCallback, useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import FormControlLabel from "@mui/material/FormControlLabel";
import IconButton from "@mui/material/IconButton";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import DeleteIcon from "@mui/icons-material/Delete";

import { ApiError, api } from "@/lib/api";
import {
  WIFI_NET_MAX,
  WIFI_PSK_MAX_BYTES,
  WIFI_PSK_MIN_BYTES,
  WIFI_SSID_MAX_BYTES,
  emptyRow,
  isValidPsk,
  isValidSsid,
  rowsToNets,
  validateRows,
  type WifiConfigResponse,
  type WifiPutRequest,
  type WifiRow,
} from "@/lib/wifi";

const DEFAULT_TLS_GUARD_MESSAGE =
  "This device is not reporting a verified TLS connection; push a CA first.";

export default function WifiPanel({ deviceId }: { deviceId: string }) {
  const [config, setConfig] = useState<WifiConfigResponse | null>(null);
  const [en, setEn] = useState(false);
  const [rows, setRows] = useState<WifiRow[]>([]);
  const [netsTouched, setNetsTouched] = useState(false);

  const [loadError, setLoadError] = useState<string | null>(null);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [validationErrors, setValidationErrors] = useState<string[]>([]);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

  function applyResponse(resp: WifiConfigResponse) {
    setConfig(resp);
    setEn(resp.en);
    setRows(resp.nets.map((n) => ({ ssid: n.s, psk: "", hadStoredPsk: n.set === true })));
    setNetsTouched(false);
  }

  // Same shape as `DevicePageClient.tsx`'s `fetchContacts`: a one-shot
  // GET-on-mount for a REST resource, only setState after the `await`.
  const fetchConfig = useCallback(async () => {
    if (!deviceId || deviceId === "_") return;
    try {
      const resp = await api.get<WifiConfigResponse>(`/devices/${deviceId}/wifi`);
      applyResponse(resp);
      setLoadError(null);
    } catch (err) {
      setLoadError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load WiFi settings."
      );
    }
  }, [deviceId]);

  useEffect(() => {
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void fetchConfig();
  }, [fetchConfig]);

  function updateRow(index: number, patch: Partial<WifiRow>) {
    setRows((prev) => {
      const next = [...prev];
      next[index] = { ...next[index], ...patch };
      return next;
    });
    setNetsTouched(true);
    setSaved(false);
  }

  function removeRow(index: number) {
    setRows((prev) => prev.filter((_, i) => i !== index));
    setNetsTouched(true);
    setSaved(false);
  }

  function addRow() {
    setRows((prev) => [...prev, emptyRow()]);
    setNetsTouched(true);
    setSaved(false);
  }

  function describeApiError(err: unknown, fallback: string): string {
    if (err instanceof ApiError) {
      if (err.status === 409) {
        return typeof err.detail === "string" ? err.detail : DEFAULT_TLS_GUARD_MESSAGE;
      }
      return String(err.detail ?? err.message);
    }
    return fallback;
  }

  async function save() {
    if (!config) return;
    const errors = netsTouched ? validateRows(rows) : [];
    setValidationErrors(errors);
    if (errors.length > 0) return;
    setSaving(true);
    setSaveError(null);
    try {
      const body: WifiPutRequest = netsTouched ? { en, nets: rowsToNets(rows) } : { en };
      const resp = await api.put<WifiConfigResponse>(`/devices/${deviceId}/wifi`, body);
      applyResponse(resp);
      setSaved(true);
    } catch (err) {
      setSaveError(describeApiError(err, "Failed to save WiFi settings."));
    } finally {
      setSaving(false);
    }
  }

  async function clearNetworks() {
    setSaving(true);
    setSaveError(null);
    setValidationErrors([]);
    try {
      const resp = await api.put<WifiConfigResponse>(`/devices/${deviceId}/wifi`, {
        en,
        nets: [],
      });
      applyResponse(resp);
      setSaved(true);
    } catch (err) {
      setSaveError(describeApiError(err, "Failed to clear WiFi networks."));
    } finally {
      setSaving(false);
    }
  }

  return (
    <Card variant="outlined">
      <CardContent>
        <Typography variant="h6" gutterBottom>
          WiFi
        </Typography>
        <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
          Carries the pager&apos;s session over WiFi instead of the cellular modem to save data. LTE
          stays the fallback. WPA2 personal only; passwords are never shown once saved.
        </Typography>

        {config?.pending && (
          <Chip size="small" color="warning" label="waiting for the pager to confirm" sx={{ mb: 2 }} />
        )}
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
        {validationErrors.length > 0 && (
          <Alert severity="error" sx={{ mb: 2 }}>
            <Stack spacing={0.5}>
              {validationErrors.map((e, i) => (
                <span key={i}>{e}</span>
              ))}
            </Stack>
          </Alert>
        )}
        {saved && validationErrors.length === 0 && !saveError && (
          <Alert severity="success" sx={{ mb: 2 }} onClose={() => setSaved(false)}>
            Saved.
          </Alert>
        )}

        {config === null ? (
          <CircularProgress size={24} />
        ) : (
          <Stack spacing={1.5}>
            <FormControlLabel
              control={
                <Switch
                  checked={en}
                  onChange={(e) => {
                    setEn(e.target.checked);
                    setSaved(false);
                  }}
                />
              }
              label="Enable WiFi"
            />

            {rows.map((row, i) => {
              const ssidOk = row.ssid.length === 0 || isValidSsid(row.ssid);
              const pskOk = row.psk.length === 0 || isValidPsk(row.psk);
              return (
                <Stack key={i} direction="row" spacing={1} sx={{ alignItems: "flex-start", flexWrap: "wrap" }}>
                  <TextField
                    size="small"
                    label="SSID"
                    value={row.ssid}
                    onChange={(e) => updateRow(i, { ssid: e.target.value })}
                    error={!ssidOk}
                    helperText={`1-${WIFI_SSID_MAX_BYTES} bytes`}
                    sx={{ minWidth: 160 }}
                  />
                  <TextField
                    size="small"
                    type="password"
                    label="Password"
                    value={row.psk}
                    onChange={(e) => updateRow(i, { psk: e.target.value })}
                    error={!pskOk}
                    placeholder={row.hadStoredPsk ? "••••••" : undefined}
                    helperText={
                      row.hadStoredPsk
                        ? "re-enter to change; required to keep this network on Save"
                        : `${WIFI_PSK_MIN_BYTES}-${WIFI_PSK_MAX_BYTES} bytes`
                    }
                    sx={{ minWidth: 200 }}
                  />
                  <IconButton aria-label="remove network" onClick={() => removeRow(i)} sx={{ mt: 0.5 }}>
                    <DeleteIcon fontSize="small" />
                  </IconButton>
                </Stack>
              );
            })}

            <Stack direction="row" spacing={2} sx={{ mt: 1 }}>
              <Button size="small" onClick={addRow} disabled={rows.length >= WIFI_NET_MAX}>
                Add network
              </Button>
              <Button size="small" variant="contained" onClick={() => void save()} disabled={saving}>
                Save
              </Button>
              <Button size="small" color="warning" onClick={() => void clearNetworks()} disabled={saving}>
                Clear networks
              </Button>
            </Stack>
            {rows.length >= WIFI_NET_MAX && (
              <Typography variant="caption" color="text.secondary">
                Maximum {WIFI_NET_MAX} networks.
              </Typography>
            )}
          </Stack>
        )}
      </CardContent>
    </Card>
  );
}

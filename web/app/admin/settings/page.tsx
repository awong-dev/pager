"use client";

/** `/admin/settings` -- docs/SERVER_PLAN.md §7.2/§5.7: retention: number +
 * days/weeks selector per class (messages, locations); a note about the
 * weekly sweep lag. */

import { doc, onSnapshot } from "firebase/firestore";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { getFirestoreDb } from "@/lib/firebase";
import type { RetentionSettingDoc, RetentionSettingsDoc, RetentionUnit } from "@/lib/types";

const DEFAULTS: RetentionSettingsDoc = {
  messages: { n: 4, unit: "weeks" },
  locations: { n: 1, unit: "weeks" },
};

function RetentionField({
  label,
  value,
  onChange,
}: {
  label: string;
  value: RetentionSettingDoc;
  onChange: (v: RetentionSettingDoc) => void;
}) {
  return (
    <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
      <Typography sx={{ minWidth: 100 }}>{label}</Typography>
      <TextField
        type="number"
        label="n"
        size="small"
        value={value.n}
        onChange={(e) => onChange({ ...value, n: Math.max(1, Number(e.target.value) || 1) })}
        sx={{ width: 100 }}
        slotProps={{ htmlInput: { min: 1 } }}
      />
      <TextField
        select
        label="unit"
        size="small"
        value={value.unit}
        onChange={(e) => onChange({ ...value, unit: e.target.value as RetentionUnit })}
        sx={{ width: 130 }}
      >
        <MenuItem value="days">days</MenuItem>
        <MenuItem value="weeks">weeks</MenuItem>
      </TextField>
    </Stack>
  );
}

function AdminSettingsInner() {
  const [settings, setSettings] = useState<RetentionSettingsDoc>(DEFAULTS);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<string | null>(null);

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubscribe = onSnapshot(doc(db, "settings", "retention"), (snap) => {
      if (snap.exists()) {
        setSettings(snap.data() as RetentionSettingsDoc);
      }
    });
    return unsubscribe;
  }, []);

  async function save() {
    setSaving(true);
    setError(null);
    setStatus(null);
    try {
      await api.put("/admin/settings", settings);
      setStatus("Saved.");
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save settings");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Stack spacing={3} sx={{ maxWidth: 480 }}>
      <Typography variant="h5">Retention</Typography>

      <RetentionField
        label="Messages"
        value={settings.messages}
        onChange={(v) => setSettings({ ...settings, messages: v })}
      />
      <RetentionField
        label="Locations"
        value={settings.locations}
        onChange={(v) => setSettings({ ...settings, locations: v })}
      />

      <Alert severity="info">
        The sweep runs weekly (docs/SERVER_PLAN.md §5.7), so a record can live up to seven days
        longer than its configured retention period before it is actually deleted.
      </Alert>

      {status && <Alert severity="success">{status}</Alert>}
      {error && <Alert severity="error">{error}</Alert>}

      <Button variant="contained" disabled={saving} onClick={() => void save()} sx={{ alignSelf: "flex-start" }}>
        {saving ? "Saving..." : "Save"}
      </Button>
    </Stack>
  );
}

export default function AdminSettingsPage() {
  return (
    <RequireAuth requireAdmin>
      <AppShell>
        <AdminSettingsInner />
      </AppShell>
    </RequireAuth>
  );
}

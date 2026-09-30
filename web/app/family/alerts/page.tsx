"use client";

/** `/family/alerts` -- docs/FAMILIES_DESIGN.md §5.4 Alerts, docs/
 * FAMILIES_TASKS.md 4.4: inbox of `families/{fam}/alerts`, newest first.
 * Open alerts always show; a "Show handled" toggle additionally loads
 * `handled`/`dismissed` ones. Each row is an `AlertCard`, which owns its own
 * per-kind approve/block/dismiss actions.
 */

import { collection, onSnapshot, orderBy, query, where } from "firebase/firestore";
import { useEffect, useState } from "react";
import FormControlLabel from "@mui/material/FormControlLabel";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import Typography from "@mui/material/Typography";

import AlertCard, { type AlertRow } from "@/components/AlertCard";
import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import type { AlertDoc } from "@/lib/types";

function FamilyAlertsInner() {
  const { familyId } = useFamily();
  const [alerts, setAlerts] = useState<AlertRow[]>([]);
  const [showHandled, setShowHandled] = useState(false);

  useEffect(() => {
    if (!familyId) return;
    const db = getFirestoreDb();
    const col = collection(db, "families", familyId, "alerts");
    const q = showHandled
      ? query(col, orderBy("ts", "desc"))
      : query(col, where("status", "==", "open"), orderBy("ts", "desc"));
    const unsubscribe = onSnapshot(q, (snap) => {
      const rows: AlertRow[] = [];
      snap.forEach((d) => rows.push({ id: d.id, ...(d.data() as AlertDoc) }));
      setAlerts(rows);
    });
    return unsubscribe;
  }, [familyId, showHandled]);

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Alerts</Typography>
        <FormControlLabel
          control={<Switch checked={showHandled} onChange={(e) => setShowHandled(e.target.checked)} />}
          label="Show handled"
        />
      </Stack>

      {alerts.length === 0 && (
        <Typography variant="body2" color="text.secondary">
          {showHandled ? "No alerts yet." : "No open alerts."}
        </Typography>
      )}

      <Stack spacing={1.5}>
        {alerts.map((a) => (
          <AlertCard key={a.id} alert={a} />
        ))}
      </Stack>
    </Stack>
  );
}

export default function FamilyAlertsPage() {
  return (
    <RequireAuth requireRole="admin">
      <AppShell>
        <FamilyAlertsInner />
      </AppShell>
    </RequireAuth>
  );
}

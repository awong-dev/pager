"use client";

/** `/admin/allowlist` -- docs/SERVER_PLAN.md §7.2/§7.5: a matrix of users x
 * users with message/locate checkboxes; save = `PUT /api/admin/allowlist`
 * (replace-all). "keep it a plain MUI Table with checkboxes and a single
 * Save."
 */

import { collection, onSnapshot } from "firebase/firestore";
import { useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { getFirestoreDb } from "@/lib/firebase";
import type { AllowEdgeDoc, UserDoc } from "@/lib/types";

interface UserRow extends UserDoc {
  uid: string;
}

interface Cell {
  message: boolean;
  locate: boolean;
}

function pairKey(from: string, to: string): string {
  return `${from}_${to}`;
}

function AllowlistInner() {
  const [users, setUsers] = useState<UserRow[]>([]);
  const [matrix, setMatrix] = useState<Map<string, Cell>>(new Map());
  const [dirty, setDirty] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubUsers = onSnapshot(collection(db, "users"), (snap) => {
      const rows: UserRow[] = [];
      snap.forEach((d) => rows.push({ uid: d.id, ...(d.data() as UserDoc) }));
      rows.sort((a, b) => a.alias.localeCompare(b.alias));
      setUsers(rows);
    });
    const unsubAllow = onSnapshot(collection(db, "allow"), (snap) => {
      setMatrix((prev) => {
        // Only overwrite from the server when there's no unsaved local edit
        // in flight, so a `Save` in progress doesn't get clobbered by the
        // listener's own echo.
        if (dirty) return prev;
        const next = new Map<string, Cell>();
        snap.forEach((d) => {
          const data = d.data() as AllowEdgeDoc;
          next.set(pairKey(data.fromUid, data.toUid), { message: data.message, locate: data.locate });
        });
        return next;
      });
    });
    return () => {
      unsubUsers();
      unsubAllow();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- `dirty` is read inside the closure intentionally, not a resubscribe trigger
  }, []);

  const pairs = useMemo(() => {
    const out: { from: UserRow; to: UserRow }[] = [];
    for (const from of users) {
      for (const to of users) {
        if (from.uid === to.uid) continue;
        out.push({ from, to });
      }
    }
    return out;
  }, [users]);

  function cellFor(from: string, to: string): Cell {
    return matrix.get(pairKey(from, to)) ?? { message: false, locate: false };
  }

  function setCell(from: string, to: string, patch: Partial<Cell>) {
    setDirty(true);
    setMatrix((prev) => {
      const next = new Map(prev);
      const key = pairKey(from, to);
      next.set(key, { ...cellFor(from, to), ...patch });
      return next;
    });
  }

  async function save() {
    setSaving(true);
    setError(null);
    try {
      const entries = pairs
        .map(({ from, to }) => ({ from, to, cell: cellFor(from.uid, to.uid) }))
        .filter(({ cell }) => cell.message || cell.locate)
        .map(({ from, to, cell }) => ({
          fromAlias: from.alias,
          toAlias: to.alias,
          message: cell.message,
          locate: cell.locate,
        }));
      await api.put("/admin/allowlist", { entries });
      setDirty(false);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save allow-list");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Allow-list</Typography>
        <Button variant="contained" disabled={!dirty || saving} onClick={() => void save()}>
          {saving ? "Saving..." : "Save"}
        </Button>
      </Stack>
      <Typography variant="body2" color="text.secondary">
        Rows are the sender (&quot;from&quot;), columns are the recipient (&quot;to&quot;).
        &quot;Message&quot; allows sending; &quot;Locate&quot; allows requesting the
        recipient&apos;s pager location.
      </Typography>
      {error && <Alert severity="error">{error}</Alert>}

      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>From \ To</TableCell>
            {users.map((u) => (
              <TableCell key={u.uid} align="center">
                @{u.alias}
              </TableCell>
            ))}
          </TableRow>
        </TableHead>
        <TableBody>
          {users.map((from) => (
            <TableRow key={from.uid}>
              <TableCell>@{from.alias}</TableCell>
              {users.map((to) => {
                if (from.uid === to.uid) {
                  return <TableCell key={to.uid} align="center">--</TableCell>;
                }
                const cell = cellFor(from.uid, to.uid);
                return (
                  <TableCell key={to.uid} align="center">
                    <Stack direction="row" spacing={0} sx={{ justifyContent: "center" }}>
                      <Checkbox
                        size="small"
                        checked={cell.message}
                        title="message"
                        onChange={(e) => setCell(from.uid, to.uid, { message: e.target.checked })}
                      />
                      <Checkbox
                        size="small"
                        checked={cell.locate}
                        title="locate"
                        onChange={(e) => setCell(from.uid, to.uid, { locate: e.target.checked })}
                      />
                    </Stack>
                  </TableCell>
                );
              })}
            </TableRow>
          ))}
        </TableBody>
      </Table>
    </Stack>
  );
}

export default function AllowlistPage() {
  return (
    <RequireAuth requireAdmin>
      <AppShell>
        <AllowlistInner />
      </AppShell>
    </RequireAuth>
  );
}

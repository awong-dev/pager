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
import FormControlLabel from "@mui/material/FormControlLabel";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { useFamily } from "@/lib/family-context";
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
  const { familyId: scopeFamilyId } = useFamily();
  const [users, setUsers] = useState<UserRow[]>([]);
  const [matrix, setMatrix] = useState<Map<string, Cell>>(new Map());
  const [dirty, setDirty] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [showAllFamilies, setShowAllFamilies] = useState(false);

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

  // docs/FAMILIES_TASKS.md 1.8: filtered to the switcher family by default;
  // "Show all families" reveals every user, including cross-family pairs
  // (whose Locate checkbox stays disabled below).
  const visibleUsers = useMemo(
    () => (showAllFamilies ? users : users.filter((u) => u.familyId === scopeFamilyId)),
    [users, showAllFamilies, scopeFamilyId]
  );

  const pairs = useMemo(() => {
    const out: { from: UserRow; to: UserRow }[] = [];
    for (const from of visibleUsers) {
      for (const to of visibleUsers) {
        if (from.uid === to.uid) continue;
        out.push({ from, to });
      }
    }
    return out;
  }, [visibleUsers]);

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
      // docs/FAMILIES_DESIGN.md §4: replace-all is scoped to `?family=`
      // when only one family is shown, so saving a filtered view never
      // wipes another family's edges; "Show all families" does a true
      // global replace.
      const path =
        !showAllFamilies && scopeFamilyId
          ? `/admin/allowlist?family=${encodeURIComponent(scopeFamilyId)}`
          : "/admin/allowlist";
      await api.put(path, { entries });
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
      <FormControlLabel
        control={
          <Switch
            checked={showAllFamilies}
            onChange={(e) => setShowAllFamilies(e.target.checked)}
          />
        }
        label="Show all families"
      />
      <Typography variant="body2" color="text.secondary">
        Rows are the viewer (&quot;from&quot;), columns are the other person (&quot;to&quot;).
        &quot;Message&quot; allows sending to that person. &quot;Locate&quot; allows seeing that
        person&apos;s pager location on /location -- their current position, their
        history for the last 7 days, and requesting a fresh fix -- until this box is
        unchecked. No one, including an admin, can grant themselves this; only an
        admin can grant it to someone else here. Locate stays within a family, so it is
        disabled between two people in different families.
      </Typography>
      {error && <Alert severity="error">{error}</Alert>}

      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>From \ To</TableCell>
            {visibleUsers.map((u) => (
              <TableCell key={u.uid} align="center">
                @{u.alias}
              </TableCell>
            ))}
          </TableRow>
        </TableHead>
        <TableBody>
          {visibleUsers.map((from) => (
            <TableRow key={from.uid}>
              <TableCell>@{from.alias}</TableCell>
              {visibleUsers.map((to) => {
                if (from.uid === to.uid) {
                  return <TableCell key={to.uid} align="center">--</TableCell>;
                }
                const cell = cellFor(from.uid, to.uid);
                const crossFamily = from.familyId !== to.familyId;
                return (
                  <TableCell key={to.uid} align="center">
                    <Stack direction="row" spacing={0} sx={{ justifyContent: "center" }}>
                      <Checkbox
                        size="small"
                        checked={cell.message}
                        title={`Allow @${from.alias} to message @${to.alias}`}
                        onChange={(e) => setCell(from.uid, to.uid, { message: e.target.checked })}
                      />
                      <Tooltip
                        title={
                          crossFamily
                            ? "Locate stays within a family"
                            : `Allow @${from.alias} to see @${to.alias}'s pager location`
                        }
                      >
                        <span>
                          <Checkbox
                            size="small"
                            checked={cell.locate}
                            disabled={crossFamily}
                            onChange={(e) => setCell(from.uid, to.uid, { locate: e.target.checked })}
                          />
                        </span>
                      </Tooltip>
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
    <RequireAuth requireRole="super">
      <AppShell>
        <AllowlistInner />
      </AppShell>
    </RequireAuth>
  );
}

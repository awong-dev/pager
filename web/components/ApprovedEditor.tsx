"use client";

/** People + Numbers editor for a member's approved list -- docs/
 * FAMILIES_DESIGN.md §5.4 People section 3. Lives in `MemberDrawer`'s
 * *Approved* section (docs/FAMILIES_TASKS.md 3.4).
 *
 * Current state is read live from Firestore (`allow where fromUid ==
 * member.uid`, readable by a family admin under the rules `firestore.rules`
 * carries by task 2.3/1.4) and joined against `useDirectory()` for each
 * edge's `toUid` -- a person-kind peer becomes a People row, an
 * external-kind peer becomes a Numbers row (its phone/name read off the
 * directory entry). An edge whose `toUid` the caller's directory cannot
 * resolve (the admin is not a party to it and shares no conversation with
 * it) is skipped rather than rendered as a bare uid -- the same "render
 * nothing, not undefined" convention `lib/types.ts` documents elsewhere.
 * Saving is a single replace-all `PUT /api/family/members/{uid}/approved`.
 */

import { useEffect, useRef, useState } from "react";
import { collection, onSnapshot, query, where } from "firebase/firestore";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import Divider from "@mui/material/Divider";
import FormControlLabel from "@mui/material/FormControlLabel";
import IconButton from "@mui/material/IconButton";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import DeleteIcon from "@mui/icons-material/Delete";

import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { familyQuery } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import { isValidPhone } from "@/lib/smsContacts";
import type { AllowEdgeDoc } from "@/lib/types";

interface PersonRow {
  message: boolean;
  locate: boolean;
}

interface NumberRow {
  phone: string;
  name: string;
}

export default function ApprovedEditor({
  uid,
  familyId,
  onSaved,
}: {
  uid: string;
  familyId: string | null;
  onSaved?: () => void;
}) {
  const directory = useDirectory();
  const [people, setPeople] = useState<Map<string, PersonRow>>(new Map());
  const [numbers, setNumbers] = useState<NumberRow[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [validationError, setValidationError] = useState<string | null>(null);
  // Only the first snapshot seeds form state -- later snapshots (e.g. this
  // save's own writeback) must not clobber in-progress edits.
  const initialized = useRef(false);

  useEffect(() => {
    const q = query(collection(getFirestoreDb(), "allow"), where("fromUid", "==", uid));
    const unsubscribe = onSnapshot(q, (snap) => {
      if (initialized.current) return;
      const nextPeople = new Map<string, PersonRow>();
      const nextNumbers: NumberRow[] = [];
      snap.forEach((d) => {
        const edge = d.data() as AllowEdgeDoc;
        const entry = directory.byUid(edge.toUid);
        if (!entry) return;
        if (entry.kind === "external") {
          if (entry.phone) nextNumbers.push({ phone: entry.phone, name: entry.displayName });
        } else {
          nextPeople.set(edge.toUid, { message: edge.message, locate: edge.locate });
        }
      });
      setPeople(nextPeople);
      setNumbers(nextNumbers);
      initialized.current = true;
    });
    return unsubscribe;
    // `directory` is a fresh object each render (its context value is
    // rebuilt from several `useMemo`s) -- keying only on `uid` avoids
    // resubscribing every render; `initialized` gates the one-time seed.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [uid]);

  const peopleContacts = directory.contacts.filter((c) => c.uid !== uid);

  function togglePerson(peerUid: string) {
    setPeople((prev) => {
      const next = new Map(prev);
      const existing = next.get(peerUid);
      if (existing?.message) {
        next.delete(peerUid);
      } else {
        next.set(peerUid, { message: true, locate: existing?.locate ?? false });
      }
      return next;
    });
  }

  function toggleLocate(peerUid: string) {
    setPeople((prev) => {
      const next = new Map(prev);
      const existing = next.get(peerUid);
      if (!existing) return prev;
      next.set(peerUid, { ...existing, locate: !existing.locate });
      return next;
    });
  }

  function addNumber() {
    setNumbers((prev) => [...prev, { phone: "", name: "" }]);
  }

  function updateNumber(i: number, patch: Partial<NumberRow>) {
    setNumbers((prev) => {
      const next = [...prev];
      next[i] = { ...next[i], ...patch };
      return next;
    });
  }

  function removeNumber(i: number) {
    setNumbers((prev) => prev.filter((_, idx) => idx !== i));
  }

  async function save() {
    setError(null);
    setValidationError(null);
    const cleanNumbers = numbers.filter((n) => n.phone.trim() || n.name.trim());
    for (const n of cleanNumbers) {
      if (!isValidPhone(n.phone)) {
        setValidationError(`"${n.phone}" is not a valid E.164 phone number.`);
        return;
      }
      if (!n.name.trim()) {
        setValidationError(`${n.phone} needs a name.`);
        return;
      }
    }
    const peoplePayload = Array.from(people.entries())
      .filter(([, row]) => row.message)
      .map(([peerUid, row]) => {
        const alias = directory.byUid(peerUid)?.alias;
        return alias ? { alias, message: true, locate: row.locate } : null;
      })
      .filter((p): p is { alias: string; message: boolean; locate: boolean } => p !== null);
    setBusy(true);
    try {
      await api.put(`/family/members/${uid}/approved${familyQuery()}`, {
        people: peoplePayload,
        numbers: cleanNumbers.map((n) => ({ phone: n.phone.trim(), name: n.name.trim() })),
      });
      initialized.current = false; // let the next snapshot re-seed from the server's result
      onSaved?.();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save approved list");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Stack spacing={2}>
      {error && <Alert severity="error">{error}</Alert>}
      {validationError && <Alert severity="warning">{validationError}</Alert>}

      <Typography variant="subtitle2">People</Typography>
      <Stack spacing={0.5}>
        {peopleContacts.map((c) => {
          const entry = directory.byUid(c.uid);
          const row = people.get(c.uid);
          const crossFamily = entry?.familyId !== familyId;
          return (
            <Stack key={c.uid} direction="row" spacing={1} sx={{ alignItems: "center" }}>
              <FormControlLabel
                sx={{ flexGrow: 1, mr: 0 }}
                control={
                  <Checkbox
                    size="small"
                    checked={row?.message ?? false}
                    onChange={() => togglePerson(c.uid)}
                  />
                }
                label={`@${c.alias}`}
              />
              <Tooltip title={crossFamily ? "Location stays within a family" : ""}>
                <span>
                  <FormControlLabel
                    control={
                      <Switch
                        size="small"
                        checked={row?.locate ?? false}
                        disabled={!row?.message || crossFamily}
                        onChange={() => toggleLocate(c.uid)}
                      />
                    }
                    label="Locate"
                  />
                </span>
              </Tooltip>
            </Stack>
          );
        })}
        {peopleContacts.length === 0 && (
          <Typography variant="body2" color="text.secondary">
            No other people known to this family yet.
          </Typography>
        )}
      </Stack>

      <Divider />

      <Typography variant="subtitle2">Numbers</Typography>
      <Stack spacing={1}>
        {numbers.map((n, i) => (
          <Stack key={i} direction="row" spacing={1} sx={{ alignItems: "flex-start" }}>
            <TextField
              size="small"
              label="Name"
              value={n.name}
              onChange={(e) => updateNumber(i, { name: e.target.value })}
              sx={{ minWidth: 140 }}
            />
            <TextField
              size="small"
              label="Phone"
              value={n.phone}
              onChange={(e) => updateNumber(i, { phone: e.target.value })}
              error={n.phone.length > 0 && !isValidPhone(n.phone)}
              helperText="e.g. +12065550100"
              sx={{ minWidth: 180 }}
            />
            <IconButton aria-label="remove number" onClick={() => removeNumber(i)} sx={{ mt: 0.5 }}>
              <DeleteIcon fontSize="small" />
            </IconButton>
          </Stack>
        ))}
        <Button size="small" onClick={addNumber} sx={{ alignSelf: "flex-start" }}>
          Add number
        </Button>
        <Typography variant="caption" color="text.secondary">
          The pager itself lists at most 8 numbers.
        </Typography>
      </Stack>

      <Button variant="contained" onClick={() => void save()} disabled={busy} sx={{ alignSelf: "flex-start" }}>
        Save approved list
      </Button>
    </Stack>
  );
}

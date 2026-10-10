"use client";

/** People + Contacts editor for a member's approved list -- docs/
 * FAMILIES_DESIGN.md §5.4 People section 3 and docs/bench-logs
 * DESIGN_no_relay_sms.md decision 11. Lives in `MemberDrawer`'s *Approved*
 * section (docs/FAMILIES_TASKS.md 3.4).
 *
 * State is seeded from `GET /api/family/members/{uid}/approved`
 * (`{people:[{alias,message,locate}], contacts:[{uid,message}]}`). Contacts
 * are SMS contacts picked from `GET /api/family/contacts`; no free-form
 * numbers are typed here (add one under Family -> Contacts). Saving is a
 * single replace-all `PUT` of the same shape, whose response re-seeds the
 * form. A member whose outbound policy is Open gets every family contact
 * implied, so a contact picked then is a deny (`message:false`).
 */

import { useEffect, useState } from "react";
import Link from "next/link";
import Alert from "@mui/material/Alert";
import Autocomplete from "@mui/material/Autocomplete";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import Chip from "@mui/material/Chip";
import Divider from "@mui/material/Divider";
import FormControlLabel from "@mui/material/FormControlLabel";
import MuiLink from "@mui/material/Link";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";

import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { familyQuery } from "@/lib/family-context";

interface PersonRow {
  message: boolean;
  locate: boolean;
}

interface ApprovedResponse {
  people: { alias: string; message: boolean; locate: boolean }[];
  contacts: { uid: string; message: boolean }[];
}

interface FamilyContact {
  uid: string;
  displayName: string;
  phone: string;
}

interface SelectedContact extends FamilyContact {
  message: boolean;
}

export default function ApprovedEditor({
  uid,
  familyId,
  policyOut,
  onSaved,
}: {
  uid: string;
  familyId: string | null;
  /** The member's saved `policy.out`. */
  policyOut: string;
  onSaved?: () => void;
}) {
  const directory = useDirectory();
  // Keyed by alias: the approved response names people by alias.
  const [people, setPeople] = useState<Map<string, PersonRow>>(new Map());
  const [allContacts, setAllContacts] = useState<FamilyContact[]>([]);
  const [selected, setSelected] = useState<SelectedContact[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const impliedAll = policyOut === "open" || policyOut === "any_sms";

  // Seed the form from a server `approved` result and the family's contacts.
  function seed(a: ApprovedResponse, contacts: FamilyContact[]) {
    const nextPeople = new Map<string, PersonRow>();
    for (const p of a.people) nextPeople.set(p.alias, { message: p.message, locate: p.locate });
    setPeople(nextPeople);
    const byUid = new Map(contacts.map((c) => [c.uid, c]));
    const nextSel: SelectedContact[] = [];
    for (const c of a.contacts) {
      const full = byUid.get(c.uid);
      if (full) nextSel.push({ ...full, message: c.message });
    }
    setSelected(nextSel);
  }

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const [a, c] = await Promise.all([
          api.get<ApprovedResponse>(`/family/members/${uid}/approved${familyQuery()}`),
          api.get<FamilyContact[]>(`/family/contacts${familyQuery()}`),
        ]);
        if (cancelled) return;
        const list = Array.isArray(c) ? c : [];
        setAllContacts(list);
        seed(a, list);
      } catch (err) {
        if (cancelled) return;
        setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load approved list");
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [uid]);

  const peopleContacts = directory.contacts.filter((c) => c.uid !== uid);

  function togglePerson(alias: string) {
    setPeople((prev) => {
      const next = new Map(prev);
      const existing = next.get(alias);
      if (existing?.message) {
        next.delete(alias);
      } else {
        next.set(alias, { message: true, locate: existing?.locate ?? false });
      }
      return next;
    });
  }

  function toggleLocate(alias: string) {
    setPeople((prev) => {
      const next = new Map(prev);
      const existing = next.get(alias);
      if (!existing) return prev;
      next.set(alias, { ...existing, locate: !existing.locate });
      return next;
    });
  }

  function pickContacts(picked: FamilyContact[]) {
    setSelected((prev) => {
      const old = new Map(prev.map((c) => [c.uid, c]));
      return picked.map((c) => old.get(c.uid) ?? { ...c, message: !impliedAll });
    });
  }

  function toggleDeny(cuid: string) {
    setSelected((prev) => prev.map((c) => (c.uid === cuid ? { ...c, message: !c.message } : c)));
  }

  async function save() {
    setError(null);
    const peoplePayload = Array.from(people.entries())
      .filter(([, row]) => row.message)
      .map(([alias, row]) => ({ alias, message: true, locate: row.locate }));
    setBusy(true);
    try {
      const resp = await api.put<ApprovedResponse>(`/family/members/${uid}/approved${familyQuery()}`, {
        people: peoplePayload,
        contacts: selected.map((c) => ({ uid: c.uid, message: c.message })),
      });
      if (resp && Array.isArray(resp.people) && Array.isArray(resp.contacts)) seed(resp, allContacts);
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

      <Typography variant="subtitle2">People</Typography>
      <Stack spacing={0.5}>
        {peopleContacts.map((c) => {
          const entry = directory.byUid(c.uid);
          const row = people.get(c.alias);
          const crossFamily = entry?.familyId !== familyId;
          return (
            <Stack key={c.uid} direction="row" spacing={1} useFlexGap sx={{ flexWrap: "wrap", alignItems: "center" }}>
              <FormControlLabel
                sx={{ flexGrow: 1, mr: 0 }}
                control={
                  <Checkbox
                    size="small"
                    checked={row?.message ?? false}
                    onChange={() => togglePerson(c.alias)}
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
                        onChange={() => toggleLocate(c.alias)}
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

      <Typography variant="subtitle2">Contacts</Typography>
      <Stack spacing={1}>
        {impliedAll && (
          <Alert severity="info">
            This member&apos;s policy is Open: every family contact is on their pager automatically. Pick a
            contact here only to deny it.
          </Alert>
        )}
        <Autocomplete<FamilyContact, true>
          multiple
          size="small"
          options={allContacts}
          value={selected}
          getOptionLabel={(c) => `${c.displayName} · ${c.phone}`}
          isOptionEqualToValue={(a, b) => a.uid === b.uid}
          onChange={(_, v) => pickContacts(v)}
          renderValue={(vals, getItemProps) =>
            vals.map((v, index) => {
              const { key, ...itemProps } = getItemProps({ index });
              const sel = selected.find((c) => c.uid === v.uid);
              const denied = sel ? !sel.message : false;
              return (
                <Chip
                  key={key}
                  size="small"
                  {...itemProps}
                  color={denied ? "error" : "default"}
                  label={`${denied ? "Denied: " : ""}${v.displayName}`}
                  onClick={() => toggleDeny(v.uid)}
                  title={denied ? "Click to allow" : "Click to deny"}
                />
              );
            })
          }
          renderInput={(params) => <TextField {...params} label="Contacts" />}
        />
        <Typography variant="caption" color="text.secondary">
          Click a chip to toggle Deny.
        </Typography>
        <MuiLink component={Link} href="/family/contacts" variant="body2">
          Add a contact under Family → Contacts
        </MuiLink>
        <Typography variant="caption" color="text.secondary">
          The pager itself lists at most 8 contacts.
        </Typography>
      </Stack>

      <Button variant="contained" onClick={() => void save()} disabled={busy} sx={{ alignSelf: "flex-start" }}>
        Save approved list
      </Button>
    </Stack>
  );
}

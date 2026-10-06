"use client";

/** Row-click drawer on `/family/people` -- docs/FAMILIES_DESIGN.md §5.4
 * People, three sections: *Profile* (`PATCH /api/family/members/{uid}`),
 * *Policy* (`PolicyPicker`, `PATCH .../{uid} {policy}`) and *Approved*
 * (`ApprovedEditor`, `PUT .../{uid}/approved`) -- docs/FAMILIES_TASKS.md 3.4.
 */

import { useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Divider from "@mui/material/Divider";
import Drawer from "@mui/material/Drawer";
import FormControlLabel from "@mui/material/FormControlLabel";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import ApprovedEditor from "@/components/ApprovedEditor";
import PolicyPicker from "@/components/PolicyPicker";
import { ApiError, api } from "@/lib/api";
import { familyQuery } from "@/lib/family-context";
import type { Role, UserDoc } from "@/lib/types";

export interface MemberDrawerMember extends UserDoc {
  uid: string;
}

// Keyed by `member.uid` from the parent below, so a newly-selected member's
// form state initializes fresh from `useState`'s lazy value instead of an
// effect syncing props into state (react.dev/learn/you-might-not-need-an-
// effect, the same convention `web/app/location/page.tsx` follows).
function MemberProfileForm({
  member,
  onClose,
}: {
  member: MemberDrawerMember;
  onClose: () => void;
}) {
  const [displayName, setDisplayName] = useState(member.displayName);
  const [role, setRole] = useState<Role>(member.role);
  const [disabled, setDisabled] = useState(member.disabled);
  // The saved outbound policy (not the unsaved radio), so the Approved hint
  // matches what the relay applies.
  const [savedOut, setSavedOut] = useState(member.policy.out);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save() {
    setBusy(true);
    setError(null);
    try {
      await api.patch(`/family/members/${member.uid}${familyQuery()}`, {
        displayName,
        role,
        disabled,
      });
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save");
    } finally {
      setBusy(false);
    }
  }

  return (
    <>
      <Typography variant="h6">@{member.alias}</Typography>

      <Typography variant="subtitle2">Profile</Typography>
      {error && <Alert severity="error">{error}</Alert>}
      <TextField
        label="Display name"
        value={displayName}
        onChange={(e) => setDisplayName(e.target.value)}
        fullWidth
      />
      <TextField
        select
        label="Role"
        value={role}
        onChange={(e) => setRole(e.target.value as Role)}
        // A family admin can only promote/demote member<->admin
        // (`relay/app/routers/family.py`'s `PATCH /members/{uid}`); super
        // is set elsewhere (`/admin/users`).
        disabled={member.role === "super"}
        fullWidth
      >
        <MenuItem value="member">Member</MenuItem>
        <MenuItem value="admin">Admin</MenuItem>
        {member.role === "super" && <MenuItem value="super">Super</MenuItem>}
      </TextField>
      <FormControlLabel
        control={<Switch checked={!disabled} onChange={(e) => setDisabled(!e.target.checked)} />}
        label={disabled ? "Disabled" : "Enabled"}
      />
      <Button variant="contained" onClick={() => void save()} disabled={busy}>
        Save
      </Button>

      <Divider />

      <Typography variant="subtitle2">Policy</Typography>
      <PolicyPicker uid={member.uid} policy={member.policy} onSaved={(p) => setSavedOut(p.out)} />

      <Divider />

      <Typography variant="subtitle2">Approved</Typography>
      <ApprovedEditor uid={member.uid} familyId={member.familyId} policyOut={savedOut} />
    </>
  );
}

export default function MemberDrawer({
  member,
  onClose,
}: {
  /** `null` closes the drawer. */
  member: MemberDrawerMember | null;
  onClose: () => void;
}) {
  return (
    <Drawer anchor="right" open={member !== null} onClose={onClose}>
      <Stack spacing={2} sx={{ width: 420, maxWidth: "100vw", overflowY: "auto", p: 3 }}>
        {member && <MemberProfileForm key={member.uid} member={member} onClose={onClose} />}
      </Stack>
    </Drawer>
  );
}

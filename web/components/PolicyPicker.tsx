"use client";

/** Two five-way radio pickers (Outbound/Inbound) for a member's conversation
 * policy -- docs/FAMILIES_DESIGN.md §2, §5.4 People section 2. Lives in
 * `MemberDrawer`'s *Policy* section; owns its own Save so it can be used
 * standalone from the drawer's Profile form (docs/FAMILIES_TASKS.md 3.4).
 */

import { useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import FormControl from "@mui/material/FormControl";
import FormControlLabel from "@mui/material/FormControlLabel";
import FormLabel from "@mui/material/FormLabel";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import { ApiError, api } from "@/lib/api";
import { familyQuery } from "@/lib/family-context";
import { INBOUND_POLICIES, OUTBOUND_POLICIES } from "@/lib/policy";
import type { PolicyDoc } from "@/lib/types";

export default function PolicyPicker({
  uid,
  policy,
  onSaved,
}: {
  uid: string;
  policy: PolicyDoc;
  onSaved?: () => void;
}) {
  const [out, setOut] = useState(policy.out);
  const [inbound, setInbound] = useState(policy.in);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save() {
    setBusy(true);
    setError(null);
    try {
      await api.patch(`/family/members/${uid}${familyQuery()}`, {
        policy: { out, in: inbound },
      });
      onSaved?.();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save policy");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Stack spacing={2}>
      {error && <Alert severity="error">{error}</Alert>}

      <FormControl>
        <FormLabel>Outbound</FormLabel>
        <RadioGroup value={out} onChange={(e) => setOut(e.target.value)}>
          {OUTBOUND_POLICIES.map((o) => (
            <FormControlLabel
              key={o.code}
              value={o.code}
              control={<Radio size="small" />}
              label={
                <Stack spacing={0}>
                  <Typography variant="body2">{o.label}</Typography>
                  <Typography variant="caption" color="text.secondary">
                    {o.description}
                  </Typography>
                </Stack>
              }
            />
          ))}
        </RadioGroup>
      </FormControl>

      <FormControl>
        <FormLabel>Inbound</FormLabel>
        <RadioGroup value={inbound} onChange={(e) => setInbound(e.target.value)}>
          {INBOUND_POLICIES.map((o) => (
            <FormControlLabel
              key={o.code}
              value={o.code}
              control={<Radio size="small" />}
              label={
                <Stack spacing={0}>
                  <Typography variant="body2">{o.label}</Typography>
                  <Typography variant="caption" color="text.secondary">
                    {o.description}
                  </Typography>
                </Stack>
              }
            />
          ))}
        </RadioGroup>
      </FormControl>

      <Button variant="contained" onClick={() => void save()} disabled={busy} sx={{ alignSelf: "flex-start" }}>
        Save policy
      </Button>
    </Stack>
  );
}

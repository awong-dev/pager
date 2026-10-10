"use client";

/** Roster table for a bridged Google Chat group -- docs/BRIDGE_PHONE_DESIGN.md
 * decision 7: each person seen speaking gets a nick (the pager's `sndr`, which
 * has the alias shape on the wire). Defaults come from `slugNick`; the nick is
 * validated inline against `NICK_RE` and for uniqueness.
 */

import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import { NICK_RE } from "@/lib/bridges";
import type { RosterEntry } from "@/lib/types";
import { responsiveTableSx } from "@/lib/tableSx";

/** One error (or null) per roster row. */
export function rosterErrors(roster: RosterEntry[]): (string | null)[] {
  const counts = new Map<string, number>();
  for (const r of roster) counts.set(r.nick, (counts.get(r.nick) ?? 0) + 1);
  return roster.map((r) => {
    if (!NICK_RE.test(r.nick)) return "Lowercase letters, digits, - or _; up to 16";
    if ((counts.get(r.nick) ?? 0) > 1) return "Must be unique";
    return null;
  });
}

export default function ChatRosterEditor({
  roster,
  onChange,
}: {
  roster: RosterEntry[];
  onChange: (next: RosterEntry[]) => void;
}) {
  const errors = rosterErrors(roster);
  return (
    <Stack spacing={1}>
      <TableContainer sx={responsiveTableSx([])}>
        {/* Hidden below md: none hidden. */}
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Name in Chat</TableCell>
              <TableCell>Name on the pager</TableCell>
            </TableRow>
          </TableHead>
          <TableBody>
            {roster.length === 0 && (
              <TableRow>
                <TableCell colSpan={2}>
                  <Typography variant="body2" color="text.secondary">
                    Nobody has spoken in this conversation yet.
                  </Typography>
                </TableCell>
              </TableRow>
            )}
            {roster.map((r, i) => (
              <TableRow key={r.name}>
                <TableCell>{r.name}</TableCell>
                <TableCell>
                  <TextField
                    size="small"
                    value={r.nick}
                    error={errors[i] !== null}
                    helperText={errors[i] ?? undefined}
                    onChange={(e) => onChange(roster.map((x, j) => (j === i ? { ...x, nick: e.target.value } : x)))}
                    slotProps={{ htmlInput: { "aria-label": `Pager name for ${r.name}` } }}
                  />
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </TableContainer>
      <Typography variant="caption" color="text.secondary">
        People who have not spoken yet appear here when they do.
      </Typography>
    </Stack>
  );
}

"use client";

/** "Soracom SIMs" on `/admin/devices` -- docs/SORACOM_DESIGN.md §5-6: lists
 * the account's SIMs and moves them into the Beam group via the relay.
 */

import { useCallback, useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Snackbar from "@mui/material/Snackbar";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";

import { ApiError } from "@/lib/api";
import { enrollAllSims, enrollSim, listSoracomSims, maskImsi, type SoracomSimsResponse } from "@/lib/soracom";

function errText(e: unknown, fallback: string): string {
  return e instanceof ApiError ? String(e.detail ?? e.message) : fallback;
}

export default function SoracomSimsSection() {
  const [data, setData] = useState<SoracomSimsResponse | null>(null);
  const [busy, setBusy] = useState(false);
  const [snack, setSnack] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setData(await listSoracomSims());
    } catch (e) {
      setSnack(errText(e, "Could not load Soracom SIMs"));
    }
  }, []);

  useEffect(() => {
    const t = setTimeout(() => void refresh(), 0);
    return () => clearTimeout(t);
  }, [refresh]);

  async function act(fn: () => Promise<unknown>, fallback: string) {
    setBusy(true);
    try {
      await fn();
    } catch (e) {
      setSnack(errText(e, fallback));
    }
    await refresh();
    setBusy(false);
  }

  const sims = data?.sims ?? [];
  const group = data?.group ?? "pager-beam";
  const unenrolled = sims.filter((s) => !s.enrolled).length;

  return (
    <Stack spacing={2} sx={{ mt: 4 }}>
      <Stack direction="row" spacing={1} sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Soracom SIMs</Typography>
        <Stack direction="row" spacing={1}>
          <Button onClick={() => void refresh()} disabled={busy}>
            Refresh
          </Button>
          <Button
            variant="contained"
            disabled={busy || unenrolled === 0}
            onClick={() => void act(enrollAllSims, "Could not enroll the SIMs")}
          >
            Enroll all
          </Button>
        </Stack>
      </Stack>
      <Typography variant="body2" color="text.secondary">
        A Soracom SIM must be enrolled before the pager boots on it: an unenrolled SIM cannot reach Beam. The pager
        needs firmware v1.1.0 or later.
      </Typography>

      {data && !data.configured ? (
        <Alert severity="info">
          Soracom enrollment is not configured. Add the SAM auth key to Secret Manager and enable it (infra/README.md,
          &quot;Soracom enrollment key&quot;).
        </Alert>
      ) : (
        <TableContainer sx={{ overflowX: "auto" }}>
          <Table size="small">
            <TableHead>
              <TableRow>
                <TableCell>ICCID</TableCell>
                <TableCell>IMSI</TableCell>
                <TableCell>Name</TableCell>
                <TableCell>Status</TableCell>
                <TableCell>Beam group</TableCell>
                <TableCell />
              </TableRow>
            </TableHead>
            <TableBody>
              {data && sims.length === 0 && (
                <TableRow>
                  <TableCell colSpan={6}>
                    <Typography variant="body2" color="text.secondary">
                      No SIMs on this Soracom account.
                    </Typography>
                  </TableCell>
                </TableRow>
              )}
              {sims.map((s) => (
                <TableRow key={s.imsi}>
                  <TableCell>{s.iccid || s.imsi}</TableCell>
                  <TableCell>
                    <Tooltip title={s.imsi}>
                      <span>{maskImsi(s.imsi)}</span>
                    </Tooltip>
                  </TableCell>
                  <TableCell>{s.name || "--"}</TableCell>
                  <TableCell>{s.status || "--"}</TableCell>
                  <TableCell>
                    {s.enrolled ? (
                      <Chip size="small" color="success" label={group} />
                    ) : (
                      <Chip size="small" color="warning" label="not enrolled" />
                    )}
                  </TableCell>
                  <TableCell>
                    {!s.enrolled && (
                      <Button
                        size="small"
                        disabled={busy}
                        onClick={() => void act(() => enrollSim(s.imsi), "Could not enroll the SIM")}
                      >
                        Enroll
                      </Button>
                    )}
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </TableContainer>
      )}

      <Snackbar open={snack !== null} autoHideDuration={8000} onClose={() => setSnack(null)}>
        <Alert severity="error" onClose={() => setSnack(null)}>
          {snack}
        </Alert>
      </Snackbar>
    </Stack>
  );
}

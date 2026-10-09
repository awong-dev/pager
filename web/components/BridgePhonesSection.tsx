"use client";

/** "Bridge phones" on `/family/devices` -- docs/BRIDGE_PHONE_DESIGN.md
 * decisions 1-3, 12 and O1 (revised): a table of the family's bridge phones
 * (polled every 10 s; bridge docs are server-only, so no Firestore listener),
 * Add bridge phone -> pairing panel, Reassign, Edit numbers (SIM and Voice,
 * either optional), Accept SIM, New code, Unpair.
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import { useCallback, useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogContentText from "@mui/material/DialogContentText";
import DialogTitle from "@mui/material/DialogTitle";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import BridgePairPanel from "@/components/BridgePairPanel";
import { ApiError } from "@/lib/api";
import {
  acceptSim,
  createBridge,
  listBridges,
  newBridgeCode,
  patchBridge,
  toMs,
  unpairBridge,
} from "@/lib/bridges";
import { getFirestoreDb } from "@/lib/firebase";
import { formatRelativeAge } from "@/lib/time";
import type { ApiTime, BridgeRow } from "@/lib/types";

export interface BridgeMember {
  uid: string;
  alias: string;
  displayName: string;
}

const POLL_MS = 10_000;
const STALE_MS = 15 * 60_000;

interface PairState {
  bridgeId: string;
  code: string;
  expiresAt: ApiTime;
}

function errText(e: unknown, fallback: string): string {
  return e instanceof ApiError ? String(e.detail ?? e.message) : fallback;
}

function StatusChip({ label, ok }: { label: string; ok: boolean | undefined }) {
  return <Chip size="small" label={label} color={ok ? "success" : "default"} variant={ok ? "filled" : "outlined"} />;
}

export default function BridgePhonesSection({
  familyId,
  members,
}: {
  familyId: string | null;
  members: BridgeMember[];
}) {
  const [bridges, setBridges] = useState<BridgeRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [now, setNow] = useState(() => Date.now());
  // The relay-derived SMS number of each family member (bridge pair sets it).
  const [smsByUid, setSmsByUid] = useState<Record<string, string | null>>({});

  const [addOpen, setAddOpen] = useState(false);
  const [addOwner, setAddOwner] = useState("");
  const [addLabel, setAddLabel] = useState("");
  const [addBusy, setAddBusy] = useState(false);
  const [addError, setAddError] = useState<string | null>(null);

  const [pair, setPair] = useState<PairState | null>(null);
  const [reassign, setReassign] = useState<{ id: string; owner: string } | null>(null);
  const [numbers, setNumbers] = useState<{ id: string; sim: string; voice: string } | null>(null);
  const [unpair, setUnpair] = useState<BridgeRow | null>(null);
  const [dialogBusy, setDialogBusy] = useState(false);
  const [dialogError, setDialogError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setBridges(await listBridges());
      setError(null);
    } catch (e) {
      setError(errText(e, "Could not load bridge phones"));
    }
  }, []);

  // Poll while the page is open; also tick the clock for "last seen" colours.
  useEffect(() => {
    if (!familyId) return;
    const first = setTimeout(() => void refresh(), 0);
    const poll = setInterval(() => void refresh(), POLL_MS);
    const tick = setInterval(() => setNow(Date.now()), 30_000);
    return () => {
      clearTimeout(first);
      clearInterval(poll);
      clearInterval(tick);
    };
  }, [familyId, refresh]);

  useEffect(() => {
    if (!familyId) return;
    const q = query(collection(getFirestoreDb(), "users"), where("familyId", "==", familyId));
    return onSnapshot(q, (snap) => {
      const next: Record<string, string | null> = {};
      snap.forEach((d) => {
        next[d.id] = (d.data().smsNumber as string | null | undefined) ?? null;
      });
      setSmsByUid(next);
    });
  }, [familyId]);

  const memberLabel = useMemo(() => {
    const m = new Map(members.map((x) => [x.uid, `${x.displayName} (@${x.alias})`]));
    return (uid: string) => m.get(uid) ?? uid.slice(0, 8);
  }, [members]);

  const pairBridge = pair ? bridges.find((b) => b.id === pair.bridgeId) : undefined;

  async function run(fn: () => Promise<void>, done: () => void) {
    setDialogBusy(true);
    setDialogError(null);
    try {
      await fn();
      done();
      await refresh();
    } catch (e) {
      setDialogError(errText(e, "Request failed"));
    } finally {
      setDialogBusy(false);
    }
  }

  async function add() {
    setAddBusy(true);
    setAddError(null);
    try {
      const res = await createBridge(addOwner, addLabel.trim());
      setAddOpen(false);
      setAddLabel("");
      setPair({ bridgeId: res.bridge.id, code: res.code, expiresAt: res.expiresAt });
      setBridges((prev) => [...prev.filter((b) => b.id !== res.bridge.id), res.bridge]);
    } catch (e) {
      setAddError(errText(e, "Could not add the bridge phone"));
    } finally {
      setAddBusy(false);
    }
  }

  async function reissue(b: BridgeRow) {
    try {
      const res = await newBridgeCode(b.id);
      setPair({ bridgeId: b.id, code: res.code, expiresAt: res.expiresAt });
    } catch (e) {
      setError(errText(e, "Could not issue a new code"));
    }
  }

  async function accept(b: BridgeRow) {
    try {
      await acceptSim(b.id);
      await refresh();
    } catch (e) {
      setError(errText(e, "Could not accept the SIM number"));
    }
  }

  const memberSelect = (value: string, onChange: (v: string) => void) => (
    <TextField select fullWidth margin="dense" label="Member" value={value} onChange={(e) => onChange(e.target.value)}>
      {members.map((m) => (
        <MenuItem key={m.uid} value={m.uid}>
          {m.displayName} (@{m.alias})
        </MenuItem>
      ))}
    </TextField>
  );

  return (
    <Stack spacing={2}>
      <Stack direction="row" sx={{ justifyContent: "space-between", alignItems: "center" }}>
        <Typography variant="h5">Bridge phones</Typography>
        <Button
          variant="contained"
          onClick={() => {
            setAddOwner(members[0]?.uid ?? "");
            setAddError(null);
            setAddOpen(true);
          }}
        >
          Add bridge phone
        </Button>
      </Stack>
      <Typography variant="body2" color="text.secondary">
        A headless Android phone that carries a member&apos;s Google Chat, Google Voice and SIM texts.
      </Typography>
      {error && <Alert severity="error">{error}</Alert>}

      <TableContainer sx={{ overflowX: "auto" }}>
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Label</TableCell>
              <TableCell>Member</TableCell>
              <TableCell>Google account</TableCell>
              <TableCell>SIM number</TableCell>
              <TableCell>Voice number</TableCell>
              <TableCell>Last seen</TableCell>
              <TableCell>Battery</TableCell>
              <TableCell>Status</TableCell>
              <TableCell>Tier 2</TableCell>
              <TableCell />
            </TableRow>
          </TableHead>
          <TableBody>
            {bridges.length === 0 && (
              <TableRow>
                <TableCell colSpan={10}>
                  <Typography variant="body2" color="text.secondary">
                    No bridge phones yet.
                  </Typography>
                </TableCell>
              </TableRow>
            )}
            {bridges.map((b) => {
              const st = b.status ?? {};
              const seen = toMs(b.lastSeenAt);
              const unpaired = !b.paired;
              const stale = !unpaired && (seen === null || now - seen > STALE_MS);
              const sim = b.simNumber ?? null;
              const voice = b.voiceNumber ?? null;
              const smsNumber = smsByUid[b.ownerUid] ?? null;
              const simMismatch = st.simNumber && st.simNumber !== sim;
              return (
                <TableRow key={b.id}>
                  <TableCell>{b.label}</TableCell>
                  <TableCell>
                    {b.ownerName ? `${b.ownerName} (@${b.ownerAlias})` : memberLabel(b.ownerUid)}
                    <Typography variant="caption" color="text.secondary" sx={{ display: "block" }}>
                      SMS number: {smsNumber ?? "none"}
                    </Typography>
                  </TableCell>
                  <TableCell>{st.accounts?.join(", ") || "--"}</TableCell>
                  <TableCell>
                    {sim ?? "--"}
                    {simMismatch && (
                      <Typography variant="caption" color="warning.main" sx={{ display: "block" }}>
                        phone reports {st.simNumber}
                      </Typography>
                    )}
                  </TableCell>
                  <TableCell>
                    {voice ?? "--"}
                    {!sim && <Chip size="small" color="info" label="Voice only" sx={{ ml: 0.5 }} />}
                  </TableCell>
                  <TableCell sx={{ color: stale ? "error.main" : undefined }}>
                    {unpaired ? "unpaired" : seen === null ? "never" : formatRelativeAge(seen)}
                  </TableCell>
                  <TableCell>{st.battery != null ? `${st.battery}%` : "--"}</TableCell>
                  <TableCell>
                    <Stack direction="row" spacing={0.5} useFlexGap sx={{ flexWrap: "wrap" }}>
                      <StatusChip label="listener" ok={st.listenerBound} />
                      {sim && <StatusChip label="SMS app" ok={st.smsDefault} />}
                      <StatusChip label="accessibility" ok={st.accessibility} />
                      {b.caps?.whatsapp && <Chip size="small" color="success" label="WhatsApp" />}
                      {st.error && <Chip size="small" color="error" label={st.error} />}
                    </Stack>
                  </TableCell>
                  <TableCell>{st.tier2Count ?? 0}</TableCell>
                  <TableCell>
                    <Stack direction="row" spacing={0.5}>
                      {unpaired ? (
                        <Button size="small" onClick={() => void reissue(b)}>
                          New code
                        </Button>
                      ) : null}
                      <Button size="small" onClick={() => setNumbers({ id: b.id, sim: sim ?? "", voice: voice ?? "" })}>
                        Numbers
                      </Button>
                      {simMismatch && (
                        <Button size="small" onClick={() => void accept(b)}>
                          Accept SIM
                        </Button>
                      )}
                      <Button size="small" onClick={() => setReassign({ id: b.id, owner: b.ownerUid })}>
                        Reassign
                      </Button>
                      <Button size="small" color="error" onClick={() => setUnpair(b)} disabled={unpaired}>
                        Unpair
                      </Button>
                    </Stack>
                  </TableCell>
                </TableRow>
              );
            })}
          </TableBody>
        </Table>
      </TableContainer>

      <Dialog open={addOpen} onClose={() => setAddOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Add bridge phone</DialogTitle>
        <DialogContent>
          {addError && <Alert severity="error" sx={{ mb: 1 }}>{addError}</Alert>}
          {memberSelect(addOwner, setAddOwner)}
          <TextField
            fullWidth
            margin="dense"
            label="Label"
            value={addLabel}
            onChange={(e) => setAddLabel(e.target.value)}
            helperText="For example: Pixel in the hall closet"
          />
          <Typography variant="caption" color="text.secondary">
            A phone signed into the member&apos;s own Google account posts in Chat as them.
          </Typography>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setAddOpen(false)}>Cancel</Button>
          <Button variant="contained" disabled={addBusy || !addOwner || !addLabel.trim()} onClick={() => void add()}>
            Create
          </Button>
        </DialogActions>
      </Dialog>

      {pair && pairBridge && (
        <BridgePairPanel
          bridge={pairBridge}
          memberLabel={memberLabel(pairBridge.ownerUid)}
          code={pair.code}
          expiresAt={pair.expiresAt}
          onEditNumbers={() =>
            setNumbers({ id: pairBridge.id, sim: pairBridge.simNumber ?? "", voice: pairBridge.voiceNumber ?? "" })
          }
          onClose={() => setPair(null)}
        />
      )}

      <Dialog open={reassign !== null} onClose={() => setReassign(null)} fullWidth maxWidth="xs">
        <DialogTitle>Reassign bridge phone</DialogTitle>
        <DialogContent>
          {dialogError && <Alert severity="error" sx={{ mb: 1 }}>{dialogError}</Alert>}
          {reassign && memberSelect(reassign.owner, (v) => setReassign({ ...reassign, owner: v }))}
          <Typography variant="caption" color="text.secondary">
            The old member&apos;s SMS number is cleared if it came from this phone; the new member&apos;s is set.
          </Typography>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setReassign(null)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={dialogBusy || !reassign}
            onClick={() => reassign && void run(async () => void (await patchBridge(reassign.id, { ownerUid: reassign.owner })), () => setReassign(null))}
          >
            Reassign
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={numbers !== null} onClose={() => setNumbers(null)} fullWidth maxWidth="xs">
        <DialogTitle>Phone numbers</DialogTitle>
        <DialogContent>
          {dialogError && <Alert severity="error" sx={{ mb: 1 }}>{dialogError}</Alert>}
          {numbers && (
            <>
              <TextField
                fullWidth
                margin="dense"
                label="SIM number"
                placeholder="+1 206 555 0100"
                value={numbers.sim}
                onChange={(e) => setNumbers({ ...numbers, sim: e.target.value })}
                helperText="Leave empty for a Voice-only phone."
              />
              <TextField
                fullWidth
                margin="dense"
                label="Google Voice number"
                placeholder="+1 206 555 0101"
                value={numbers.voice}
                onChange={(e) => setNumbers({ ...numbers, voice: e.target.value })}
                helperText="The member's SMS number is the SIM number, or the Voice number when there is no SIM."
              />
            </>
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setNumbers(null)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={dialogBusy || !numbers}
            onClick={() =>
              numbers &&
              void run(
                async () =>
                  void (await patchBridge(numbers.id, {
                    // "" clears, a string sets (relay PatchBridgeRequest).
                    simNumber: numbers.sim.trim(),
                    voiceNumber: numbers.voice.trim(),
                  })),
                () => setNumbers(null)
              )
            }
          >
            Save
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={unpair !== null} onClose={() => setUnpair(null)}>
        <DialogTitle>Unpair {unpair?.label}?</DialogTitle>
        <DialogContent>
          {dialogError && <Alert severity="error" sx={{ mb: 1 }}>{dialogError}</Alert>}
          <DialogContentText>
            The phone stops working immediately. Texts waiting to send fail, and the member&apos;s SMS number is
            cleared if it came from this phone. Contacts and conversations are kept.
          </DialogContentText>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setUnpair(null)}>Cancel</Button>
          <Button
            color="error"
            disabled={dialogBusy}
            onClick={() => unpair && void run(async () => void (await unpairBridge(unpair.id)), () => setUnpair(null))}
          >
            Unpair
          </Button>
        </DialogActions>
      </Dialog>
    </Stack>
  );
}

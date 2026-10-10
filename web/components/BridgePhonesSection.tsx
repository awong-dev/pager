"use client";

/** "Bridge phones" on `/family/devices` -- docs/BRIDGE_PHONE_DESIGN.md
 * decisions 1-3, 12 and O1 (revised): a card per bridge phone of the family's bridge phones
 * (polled every 10 s; bridge docs are server-only, so no Firestore listener),
 * Add bridge phone -> pairing panel, Reassign, Edit numbers (SIM and Voice,
 * either optional), Accept SIM, New code, Unpair.
 */

import { collection, onSnapshot, query, where } from "firebase/firestore";
import { useCallback, useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogContentText from "@mui/material/DialogContentText";
import DialogTitle from "@mui/material/DialogTitle";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
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
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

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

function Fact({ label, children, color }: { label: string; children: React.ReactNode; color?: string }) {
  return (
    <>
      <Typography variant="caption" color="text.secondary" sx={{ lineHeight: "24px" }}>
        {label}
      </Typography>
      <Typography variant="body2" color={color} component="div" sx={{ minHeight: 24, display: "flex", alignItems: "center", flexWrap: "wrap", gap: 0.5, overflowWrap: "anywhere", minWidth: 0 }}>
        {children}
      </Typography>
    </>
  );
}

interface BridgePhoneCardProps {
  bridge: BridgeRow;
  ownerLabel: string;
  smsNumber: string | null;
  seenLabel: string;
  stale: boolean;
  simMismatch: boolean;
  onNewCode: () => void;
  onNumbers: () => void;
  onAccept: () => void;
  onReassign: () => void;
  onUnpair: () => void;
}

function BridgePhoneCard(p: BridgePhoneCardProps) {
  const b = p.bridge;
  const st = b.status ?? {};
  const unpaired = !b.paired;
  const sim = b.simNumber ?? null;
  const voice = b.voiceNumber ?? null;
  return (
    <Card variant="outlined" sx={{ width: "100%" }}>
      <CardContent sx={{ p: 2, "&:last-child": { pb: 2 } }}>
        <Box sx={{ display: "grid", gridTemplateColumns: { xs: "1fr", md: "1fr 1fr" }, rowGap: 0.5, columnGap: 3 }}>
          <Stack
            direction="row"
            useFlexGap
            sx={{ gridColumn: "1 / -1", flexWrap: "wrap", columnGap: 1, rowGap: 0.5, alignItems: "center", minWidth: 0 }}
          >
            <Typography variant="h6" sx={{ fontWeight: 700, overflowWrap: "anywhere", lineHeight: 1.3 }}>
              {b.label}
            </Typography>
            <Typography variant="body2" color="text.secondary" sx={{ overflowWrap: "anywhere" }}>
              {p.ownerLabel}
            </Typography>
            <Chip
              size="small"
              label={p.seenLabel}
              color={p.stale ? "error" : "default"}
              variant="outlined"
            />
            <StatusChip label="listener" ok={st.listenerBound} />
            {sim && <StatusChip label="SMS app" ok={st.smsDefault} />}
            <StatusChip label="accessibility" ok={st.accessibility} />
            {b.caps?.whatsapp && <Chip size="small" color="success" label="WhatsApp" />}
          </Stack>

          <Box sx={{ display: "grid", gridTemplateColumns: "auto 1fr", columnGap: 1.5, alignItems: "center", alignContent: "start", minWidth: 0 }}>
            <Fact label="Google account">{st.accounts?.join(", ") || "--"}</Fact>
            <Fact label="SIM number">
              {sim ?? "--"}
              {p.simMismatch && (
                <Typography variant="caption" color="warning.main">
                  phone reports {st.simNumber}
                </Typography>
              )}
            </Fact>
            <Fact label="Voice number">
              {voice ?? "--"}
              {!sim && <Chip size="small" color="info" label="Voice only" />}
            </Fact>
            <Fact label="SMS number">{p.smsNumber ?? "none"}</Fact>
          </Box>

          <Box sx={{ display: "grid", gridTemplateColumns: "auto 1fr", columnGap: 1.5, alignItems: "center", alignContent: "start", minWidth: 0 }}>
            <Fact label="Battery">{st.battery != null ? `${st.battery}%` : "--"}</Fact>
            <Fact label="Tier 2">{st.tier2Count ?? 0}</Fact>
            {st.error && (
              <Fact label="Error" color="error.main">
                {st.error}
              </Fact>
            )}
            <Box sx={{ gridColumn: "1 / -1", display: "flex", flexWrap: "wrap", gap: 0.5, alignItems: "center", pt: 0.5 }}>
              {unpaired && (
                <Button size="small" variant="outlined" onClick={p.onNewCode}>
                  New code
                </Button>
              )}
              <Button size="small" variant="outlined" onClick={p.onNumbers}>
                Numbers
              </Button>
              {p.simMismatch && (
                <Button size="small" variant="outlined" onClick={p.onAccept}>
                  Accept SIM
                </Button>
              )}
              <Button size="small" variant="outlined" onClick={p.onReassign}>
                Reassign
              </Button>
              <Button size="small" color="error" onClick={p.onUnpair} disabled={unpaired} sx={{ ml: { md: "auto" } }}>
                Unpair
              </Button>
            </Box>
          </Box>
        </Box>
      </CardContent>
    </Card>
  );
}

export default function BridgePhonesSection({
  familyId,
  members,
}: {
  familyId: string | null;
  members: BridgeMember[];
}) {
  const fullScreen = useFullScreenDialog();
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
      <Stack direction="row" sx={{ flexWrap: "wrap", justifyContent: "space-between", alignItems: "center" }}>
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

      {bridges.length === 0 && (
        <Typography variant="body2" color="text.secondary">
          No bridge phones yet.
        </Typography>
      )}
      {bridges.map((b) => {
        const st = b.status ?? {};
        const seen = toMs(b.lastSeenAt);
        const unpaired = !b.paired;
        const stale = !unpaired && (seen === null || now - seen > STALE_MS);
        const sim = b.simNumber ?? null;
        const voice = b.voiceNumber ?? null;
        const simMismatch = Boolean(st.simNumber && st.simNumber !== sim);
        return (
          <BridgePhoneCard
            key={b.id}
            bridge={b}
            ownerLabel={b.ownerName ? `${b.ownerName} (@${b.ownerAlias})` : memberLabel(b.ownerUid)}
            smsNumber={smsByUid[b.ownerUid] ?? null}
            seenLabel={unpaired ? "unpaired" : seen === null ? "never" : formatRelativeAge(seen)}
            stale={stale}
            simMismatch={simMismatch}
            onNewCode={() => void reissue(b)}
            onNumbers={() => setNumbers({ id: b.id, sim: sim ?? "", voice: voice ?? "" })}
            onAccept={() => void accept(b)}
            onReassign={() => setReassign({ id: b.id, owner: b.ownerUid })}
            onUnpair={() => setUnpair(b)}
          />
        );
      })}

      <Dialog fullScreen={fullScreen} open={addOpen} onClose={() => setAddOpen(false)} fullWidth maxWidth="xs">
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

      <Dialog fullScreen={fullScreen} open={reassign !== null} onClose={() => setReassign(null)} fullWidth maxWidth="xs">
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

      <Dialog fullScreen={fullScreen} open={numbers !== null} onClose={() => setNumbers(null)} fullWidth maxWidth="xs">
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

      <Dialog fullScreen={fullScreen} open={unpair !== null} onClose={() => setUnpair(null)}>
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

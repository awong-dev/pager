"use client";

/** One pager device as a dense two-column card -- shared by `/family/devices`
 * and `/admin/devices` so the two pages do not drift. Layout only: every
 * action is a callback supplied by the page (which owns the API calls and
 * confirms).
 *
 * Header (label, id, status, Details), then Facts (left) beside
 * Firmware & actions (right); one column on xs.
 */

import Link from "next/link";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import MenuItem from "@mui/material/MenuItem";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import DeviceTrustChip from "@/components/DeviceTrustChip";
import EditDeviceLabelButton from "@/components/EditDeviceLabelDialog";
import FirmwareChip from "@/components/FirmwareChip";
import { deviceName } from "@/lib/devices";
import { locBackoffLabel } from "@/lib/deviceTrust";
import type { FirmwareBuild, FirmwareScope } from "@/lib/firmware";
import { formatRelativeAge } from "@/lib/time";
import type { DeviceDoc } from "@/lib/types";

// `devices/{d}.provisionState` (docs/DEVICE_PLAN.md §3.2/§D0.2) is not part
// of `lib/types.ts`'s `DeviceDoc` mirror.
export type ProvisionState = "issued" | "provisioned" | null | undefined;

// `devices/{d}.pendingCfg` -- `relay/app/devcfg.py`'s `_set_pending` shape.
export interface PendingCfgDoc {
  id: string;
  obj: { cfg?: { lock?: { auto?: number; clear?: boolean } } };
  acked: boolean;
}

export interface DeviceRow extends DeviceDoc {
  id: string;
  provisionState?: ProvisionState;
  pendingCfg?: PendingCfgDoc;
}

// docs/DEVICE_PLAN.md §5.8: `auto_min` is a `u8` minutes value, 0 = never;
// default 5.
const AUTO_LOCK_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Off" },
  { value: 5, label: "5 min" },
  { value: 15, label: "15 min" },
  { value: 30, label: "30 min" },
  { value: 60, label: "60 min" },
];

export interface DeviceCardProps {
  device: DeviceRow;
  scope: FirmwareScope;
  newestBuild?: FirmwareBuild;
  /** Display strings, already resolved by the page (e.g. "@alice"). */
  ownerLabel: string;
  defaultToLabel: string;
  /** Admin page only: shows a Family line. */
  familyLabel?: string;
  onRotate: () => void;
  onRevoke: () => void;
  onDelete: () => void;
  onFirmware: () => void;
  onCa: (action: "push" | "unpin") => void;
  onSetAutoLock: (minutes: number) => void;
  onClearPasscode: () => void;
}

// firmware/main/modes.c PAGER_BATT_MV_UNKNOWN_PLACEHOLDER: reported until the
// first real ADC reading lands.
const BATT_MV_UNKNOWN_PLACEHOLDER = 3700;

function Fact({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <>
      <Typography variant="caption" color="text.secondary" sx={{ lineHeight: "24px" }}>
        {label}
      </Typography>
      <Box sx={{ minWidth: 0, minHeight: 24, display: "flex", alignItems: "center", flexWrap: "wrap", gap: 0.5 }}>
        <Typography variant="body2" component="div" sx={{ overflowWrap: "anywhere" }}>
          {children}
        </Typography>
      </Box>
    </>
  );
}

function Battery({ id, mv, ts }: { id: string; mv: number | null | undefined; ts: number | null | undefined }) {
  if (mv == null) return <>–</>;
  const age = ts ? ` · ${formatRelativeAge(ts * 1000)}` : "";
  const text = mv >= 4300 ? "USB" : mv === BATT_MV_UNKNOWN_PLACEHOLDER ? "unknown" : `${mv} mV`;
  const color = mv >= 4300 || mv === BATT_MV_UNKNOWN_PLACEHOLDER ? "text.primary" : mv < 3550 ? "error" : mv < 3700 ? "warning.main" : "text.primary";
  return (
    <Link href={`/devices/${id}#battery`} style={{ color: "inherit" }}>
      <Typography component="span" variant="body2" color={color}>
        {text}
        {mv !== BATT_MV_UNKNOWN_PLACEHOLDER ? age : ""}
      </Typography>
    </Link>
  );
}

export default function DeviceCard(p: DeviceCardProps) {
  const d = p.device;
  const backoff = locBackoffLabel(d.status?.locBackoffS);
  const state = d.status?.state ?? "unknown";
  const ts = d.status?.ts;
  return (
    <Card variant="outlined" sx={{ width: "100%" }}>
      <CardContent sx={{ p: 2, "&:last-child": { pb: 2 } }}>
        <Box sx={{ display: "grid", gridTemplateColumns: { xs: "1fr", md: "1fr 1fr" }, rowGap: 0.5, columnGap: 3 }}>
          {/* Header */}
          <Stack
            direction="row"
            useFlexGap
            sx={{ gridColumn: "1 / -1", flexWrap: "wrap", columnGap: 1, rowGap: 0.5, alignItems: "center", minWidth: 0 }}
          >
            <Typography variant="h6" sx={{ fontWeight: 700, overflowWrap: "anywhere", lineHeight: 1.3 }}>
              {deviceName(d)}
            </Typography>
            <EditDeviceLabelButton device={d} scope={p.scope} />
            <Typography variant="caption" color="text.secondary" sx={{ fontFamily: "monospace", overflowWrap: "anywhere" }}>
              {d.id}
            </Typography>
            <Chip
              size="small"
              label={state}
              color={state === "online" ? "success" : "default"}
              variant={state === "online" ? "filled" : "outlined"}
            />
            {ts ? (
              <Typography variant="caption" color="text.secondary">
                {formatRelativeAge(ts * 1000)}
              </Typography>
            ) : null}
            {d.revokedAt && <Chip size="small" color="error" label="revoked" />}
            {backoff && (
              <Typography variant="caption" color="text.secondary">
                {backoff}
              </Typography>
            )}
            <Button size="small" variant="outlined" component={Link} href={`/devices/${d.id}`} sx={{ ml: "auto" }}>
              Details
            </Button>
          </Stack>

          {/* Facts */}
          <Box sx={{ display: "grid", gridTemplateColumns: "auto 1fr", columnGap: 1.5, alignItems: "center", alignContent: "start", minWidth: 0 }}>
            <Fact label="Owner">{p.ownerLabel}</Fact>
            {p.familyLabel !== undefined && <Fact label="Family">{p.familyLabel}</Fact>}
            <Fact label="Default to">{p.defaultToLabel || "--"}</Fact>
            <Fact label="Battery">
              <Battery id={d.id} mv={d.status?.battMv} ts={ts} />
            </Fact>
            <Fact label="Link">
              <DeviceTrustChip tls={d.status?.tls} caFp={d.status?.caFp} />
              {d.status?.car && <Chip size="small" variant="outlined" label={d.status.car} />}
            </Fact>
            <Fact label="Provisioned">
              {d.provisionState === "provisioned" ? "yes" : d.provisionState ? d.provisionState : "no"}
            </Fact>
            <Fact label="Lock">
              <TextField
                select
                size="small"
                hiddenLabel
                value={d.pendingCfg?.obj.cfg?.lock?.auto ?? ""}
                onChange={(e) => p.onSetAutoLock(Number(e.target.value))}
                slotProps={{ htmlInput: { "aria-label": "Auto-lock" } }}
                sx={{ width: 140, "& .MuiInputBase-input": { py: 0.5 } }}
              >
                {AUTO_LOCK_OPTIONS.map((o) => (
                  <MenuItem key={o.value} value={o.value}>
                    {o.label}
                  </MenuItem>
                ))}
              </TextField>
              <Button size="small" onClick={p.onClearPasscode}>
                Clear passcode
              </Button>
            </Fact>
          </Box>

          {/* Firmware & actions */}
          <Stack spacing={0.5} sx={{ alignItems: "flex-start", minWidth: 0 }}>
            <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 0.5, alignItems: "center" }}>
              <FirmwareChip status={d.status} newest={p.newestBuild} />
              <Button size="small" onClick={p.onFirmware}>
                Update firmware…
              </Button>
            </Stack>
            {d.status?.tls !== "proxy" && (
              <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 0.5 }}>
                <Button size="small" onClick={() => p.onCa("push")}>
                  Push CA
                </Button>
                <Button size="small" color="warning" onClick={() => p.onCa("unpin")}>
                  Un-pin CA
                </Button>
              </Stack>
            )}
            <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 0.5, width: "100%", alignItems: "center" }}>
              <Button size="small" variant="outlined" onClick={p.onRotate}>
                Rotate
              </Button>
              <Button size="small" variant="outlined" component={Link} href={`/devices/${d.id}`}>
                SMS
              </Button>
              <Button size="small" color="warning" onClick={p.onRevoke} sx={{ ml: { md: "auto" } }}>
                Revoke
              </Button>
              <Button size="small" color="error" onClick={p.onDelete}>
                Delete
              </Button>
            </Stack>
          </Stack>
        </Box>
      </CardContent>
    </Card>
  );
}

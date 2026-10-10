"use client";

/** One pager device as a full-width card -- shared by `/family/devices` and
 * `/admin/devices` so the two pages do not drift. Layout only: every action is
 * a callback supplied by the page (which owns the API calls and confirms).
 *
 * Top to bottom: header (label, id, status, Details link), firmware row,
 * actions row, then a collapsed "More" section with the secondary info.
 */

import Link from "next/link";
import { useState } from "react";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import Collapse from "@mui/material/Collapse";
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

function Line({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <Typography variant="body2" color="text.secondary" component="div">
      {label}: {children}
    </Typography>
  );
}

export default function DeviceCard(p: DeviceCardProps) {
  const d = p.device;
  const [more, setMore] = useState(false);
  const backoff = locBackoffLabel(d.status?.locBackoffS);
  const state = d.status?.state ?? "unknown";
  return (
    <Card variant="outlined" sx={{ width: "100%" }}>
      <CardContent sx={{ "&:last-child": { pb: 2 } }}>
        <Stack spacing={1.5} sx={{ alignItems: "flex-start" }}>
          {/* 1. Header */}
          <Stack
            direction="row"
            useFlexGap
            sx={{ width: "100%", flexWrap: "wrap", gap: 1, justifyContent: "space-between", alignItems: "center" }}
          >
            <Box sx={{ minWidth: 0 }}>
              <Stack direction="row" spacing={0.5} sx={{ alignItems: "center" }}>
                <Typography variant="subtitle1" sx={{ fontWeight: 700, overflowWrap: "anywhere" }}>
                  {deviceName(d)}
                </Typography>
                <EditDeviceLabelButton device={d} scope={p.scope} />
              </Stack>
              <Typography
                variant="caption"
                color="text.secondary"
                sx={{ fontFamily: "monospace", overflowWrap: "anywhere" }}
              >
                {d.id}
              </Typography>
            </Box>
            <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 1, alignItems: "center" }}>
              <Chip
                size="small"
                label={state}
                color={state === "online" ? "success" : "default"}
                variant={state === "online" ? "filled" : "outlined"}
              />
              <Button size="small" variant="outlined" component={Link} href={`/devices/${d.id}`}>
                Details
              </Button>
            </Stack>
          </Stack>
          {backoff && (
            <Typography variant="caption" color="text.secondary">
              {backoff}
            </Typography>
          )}

          {/* 2. Firmware */}
          <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 1, alignItems: "center" }}>
            <FirmwareChip status={d.status} newest={p.newestBuild} />
            <Button size="small" onClick={p.onFirmware}>
              Update firmware…
            </Button>
          </Stack>

          {/* 3. Actions */}
          <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 1 }}>
            <Button size="small" onClick={p.onRotate}>
              Rotate
            </Button>
            <Button size="small" color="warning" onClick={p.onRevoke}>
              Revoke
            </Button>
            <Button size="small" color="error" onClick={p.onDelete}>
              Delete
            </Button>
            <Button size="small" component={Link} href={`/devices/${d.id}`}>
              SMS
            </Button>
          </Stack>

          {/* 4. More */}
          <Button size="small" onClick={() => setMore((v) => !v)} aria-expanded={more}>
            {more ? "Less" : "More"}
          </Button>
          <Collapse in={more} unmountOnExit sx={{ width: "100%" }}>
            <Stack spacing={1} sx={{ alignItems: "flex-start" }}>
              {p.familyLabel !== undefined && <Line label="Family">{p.familyLabel}</Line>}
              <Line label="Owner">{p.ownerLabel}</Line>
              <Line label="Default to">{p.defaultToLabel}</Line>
              <Line label="Provisioned">
                {d.provisionState === "provisioned" ? "yes" : (d.provisionState ?? "unknown")}
              </Line>
              {d.revokedAt && <Line label="Revoked">yes</Line>}
              <Line label="Battery">
                {d.status?.battMv == null ? (
                  "–"
                ) : (
                  <Link href={`/devices/${d.id}#battery`} style={{ color: "inherit" }}>
                    <Typography
                      component="span"
                      variant="body2"
                      color={
                        d.status.battMv >= 4300
                          ? "text.primary"
                          : d.status.battMv < 3550
                            ? "error"
                            : d.status.battMv < 3700
                              ? "warning.main"
                              : "text.primary"
                      }
                    >
                      {d.status.battMv >= 4300 ? "USB" : `${d.status.battMv} mV`}
                    </Typography>
                  </Link>
                )}
              </Line>
              <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 0.5, alignItems: "center" }}>
                <Typography variant="body2" color="text.secondary">
                  CA trust:
                </Typography>
                <DeviceTrustChip tls={d.status?.tls} caFp={d.status?.caFp} />
                {d.status?.car && <Chip size="small" variant="outlined" label={d.status.car} />}
                {d.status?.tls !== "proxy" && (
                  <>
                    <Button size="small" onClick={() => p.onCa("push")}>
                      Push CA
                    </Button>
                    <Button size="small" color="warning" onClick={() => p.onCa("unpin")}>
                      Un-pin CA
                    </Button>
                  </>
                )}
              </Stack>
              <Stack direction="row" useFlexGap sx={{ flexWrap: "wrap", gap: 1, alignItems: "center" }}>
                <TextField
                  select
                  size="small"
                  label="Auto-lock"
                  value={d.pendingCfg?.obj.cfg?.lock?.auto ?? ""}
                  onChange={(e) => p.onSetAutoLock(Number(e.target.value))}
                  sx={{ minWidth: 100 }}
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
              </Stack>
            </Stack>
          </Collapse>
        </Stack>
      </CardContent>
    </Card>
  );
}

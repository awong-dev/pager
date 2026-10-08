"use client";

/** The Battery card on `/devices/[id]` (docs/BATTERY_STATS_DESIGN.md B7).
 * One GET on mount and on range change (`lib/battery.ts`); the stored
 * samples carry the pager's own state-time counters and the card models the
 * drain from them. Edits to the current table stay in local state. */

import { useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Typography from "@mui/material/Typography";

import { ApiError } from "@/lib/api";
import { fetchBattery } from "@/lib/battery";
import {
  USB_MV,
  bucketAwake,
  sampleMah,
  termBreakdown,
  toCsv,
  withTermCurrent,
  type BatteryModel,
  type BatterySample,
} from "@/lib/batteryModel";
import { AwakeStackChart, VoltageChart } from "@/components/BatteryCharts";

const RANGES = [
  { key: "24h", label: "24 h", days: 1 },
  { key: "7d", label: "7 d", days: 7 },
  { key: "30d", label: "30 d", days: 30 },
  { key: "90d", label: "90 d", days: 90 },
];

function ymd(tsS: number): string {
  const d = new Date(tsS * 1000);
  const p = (v: number) => String(v).padStart(2, "0");
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}`;
}

function fmtHours(s: number): string {
  return `${(s / 3600).toFixed(1)} h`;
}

function fmtExposure(v: number, unit: "h" | "count"): string {
  return unit === "h" ? `${v.toFixed(1)} h` : String(Math.round(v));
}

export default function BatteryCard({ deviceId }: { deviceId: string }) {
  const [rangeKey, setRangeKey] = useState("7d");
  const [samples, setSamples] = useState<BatterySample[]>([]);
  const [serverModel, setServerModel] = useState<BatteryModel | null>(null);
  const [edited, setEdited] = useState<BatteryModel | null>(null);
  const [truncated, setTruncated] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [since, setSince] = useState(0);
  const [until, setUntil] = useState(0);

  useEffect(() => {
    const days = RANGES.find((r) => r.key === rangeKey)?.days ?? 7;
    const u = Math.floor(Date.now() / 1000);
    const s = u - days * 86400;
    let cancelled = false;
    fetchBattery(deviceId, s, u)
      .then((r) => {
        if (cancelled) return;
        setSamples(r.samples);
        setTruncated(r.truncated);
        setServerModel(r.model);
        setEdited(null);
        setSince(s);
        setUntil(u);
      })
      .catch((e: unknown) => {
        if (cancelled) return;
        setError(e instanceof ApiError || e instanceof Error ? e.message : "could not load battery data");
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [deviceId, rangeKey]);

  const model = edited ?? serverModel;
  const days = RANGES.find((r) => r.key === rangeKey)?.days ?? 7;
  const bucketUnit = days <= 7 ? "hour" : "day";
  const unitLabel = days <= 7 ? "per hour" : "per day";
  const withBs = useMemo(() => samples.filter((s) => s.hasBs), [samples]);
  const buckets = useMemo(() => bucketAwake(samples, bucketUnit), [samples, bucketUnit]);
  const last = samples.length > 0 ? samples[samples.length - 1] : null;
  const lastMv = [...samples].reverse().find((s) => s.battMv != null)?.battMv;

  const totals = useMemo(() => {
    const t = { off: 0, search: 0, gnss: 0, ext1: 0, sleeps: 0, connects: 0, dt: 0 };
    for (const s of withBs) {
      t.off += s.modemS?.off ?? 0;
      t.search += s.modemS?.search ?? 0;
      t.gnss += s.modemS?.gnss ?? 0;
      t.ext1 += s.ext1 ?? 0;
      t.sleeps += s.sleeps ?? 0;
      t.connects += s.connects ?? 0;
      t.dt += s.dtS ?? 0;
    }
    return t;
  }, [withBs]);

  const refreshDays = useMemo(() => {
    const m = new Map<string, { full: number; partial: number; upgraded: number }>();
    for (const s of withBs) {
      const k = ymd(s.ts);
      const r = m.get(k) ?? { full: 0, partial: 0, upgraded: 0 };
      r.full += s.refresh?.full ?? 0;
      r.partial += s.refresh?.partial ?? 0;
      r.upgraded += s.refresh?.upgraded ?? 0;
      m.set(k, r);
    }
    return [...m.entries()].sort((a, b) => (a[0] < b[0] ? 1 : -1)).slice(0, 14);
  }, [withBs]);

  const drain = useMemo(() => {
    if (!model || totals.dt <= 0) return null;
    let mah = 0;
    for (const s of withBs) mah += sampleMah(s, model) ?? 0;
    const perDay = (mah * 86400) / totals.dt;
    return { perDay, daysFromFull: perDay > 0 ? (model.capacityMah * model.usableFrac) / perDay : null };
  }, [model, withBs, totals.dt]);

  const rows = useMemo(() => (model ? termBreakdown(samples, model) : []), [samples, model]);

  const download = () => {
    if (!model) return;
    const blob = new Blob([toCsv(samples, model)], { type: "text/csv" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `battery-${deviceId}-${ymd(since)}-${ymd(until)}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };

  const fitted =
    serverModel?.source === "fit"
      ? `model, fitted ${serverModel.fitAt ? new Date(serverModel.fitAt * 1000).toLocaleDateString() : "?"}`
      : "model, uncalibrated";

  return (
    <Card variant="outlined" id="battery">
      <CardContent>
        <Stack spacing={2}>
          <Stack direction="row" spacing={2} sx={{ alignItems: "center", flexWrap: "wrap" }}>
            <Typography variant="h6" sx={{ flexGrow: 1 }}>
              Battery
            </Typography>
            <ToggleButtonGroup
              size="small"
              exclusive
              value={rangeKey}
              onChange={(_, v: string | null) => {
                if (!v) return;
                setLoading(true);
                setError(null);
                setRangeKey(v);
              }}
            >
              {RANGES.map((r) => (
                <ToggleButton key={r.key} value={r.key}>
                  {r.label}
                </ToggleButton>
              ))}
            </ToggleButtonGroup>
          </Stack>

          {loading && <CircularProgress size={24} />}
          {error && <Alert severity="error">{error}</Alert>}
          {truncated && <Alert severity="warning">Some samples were not loaded (5000 per 30 days cap)</Alert>}

          {!loading && !error && samples.length === 0 && (
            <Typography variant="body2" color="text.secondary">
              No battery samples yet. They arrive with each status, about hourly.
            </Typography>
          )}

          {!loading && !error && samples.length > 0 && model && (
            <>
              <Typography variant="body2">
                {lastMv != null ? (lastMv >= USB_MV ? `${lastMv} mV (on USB)` : `${lastMv} mV`) : "no voltage"}
                {last && ` · last sample ${new Date(last.ts * 1000).toLocaleString()}`}
                {withBs.length < samples.length &&
                  ` · ${withBs.length} of ${samples.length} samples carry counters`}
              </Typography>

              <VoltageChart samples={samples} />

              {withBs.length > 0 && (
                <>
                  <AwakeStackChart buckets={buckets} unitLabel={unitLabel} />
                  <Typography variant="body2">
                    modem off {fmtHours(totals.off)} · searching {fmtHours(totals.search)} · GNSS{" "}
                    {(totals.gnss / 60).toFixed(1)} min
                    <br />
                    ext1 wakes {totals.ext1} · sleeps {totals.sleeps} · reconnects {totals.connects}
                  </Typography>

                  <Typography variant="subtitle2">Display refreshes per day</Typography>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Day</TableCell>
                        <TableCell align="right">Full</TableCell>
                        <TableCell align="right">Partial</TableCell>
                        <TableCell align="right">Upgraded</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {refreshDays.map(([d, r]) => (
                        <TableRow key={d}>
                          <TableCell>{`${d.slice(0, 4)}-${d.slice(4, 6)}-${d.slice(6)}`}</TableCell>
                          <TableCell align="right">{r.full}</TableCell>
                          <TableCell align="right">{r.partial}</TableCell>
                          <TableCell align="right">{r.upgraded}</TableCell>
                        </TableRow>
                      ))}
                      <TableRow>
                        <TableCell sx={{ fontWeight: "bold" }}>Total</TableCell>
                        {(["full", "partial", "upgraded"] as const).map((k) => (
                          <TableCell key={k} align="right" sx={{ fontWeight: "bold" }}>
                            {refreshDays.reduce((a, [, r]) => a + r[k], 0)}
                          </TableCell>
                        ))}
                      </TableRow>
                    </TableBody>
                  </Table>

                  <Stack direction="row" spacing={2} sx={{ alignItems: "center", flexWrap: "wrap" }}>
                    <Typography variant="subtitle1">
                      {drain
                        ? `≈ ${drain.perDay.toFixed(1)} mAh/day${
                            drain.daysFromFull != null ? ` → ≈ ${Math.round(drain.daysFromFull)} days from full` : ""
                          }`
                        : "Modelled drain unavailable"}
                    </Typography>
                    <Chip
                      size="small"
                      label={fitted}
                      color={serverModel?.source === "fit" ? "success" : "warning"}
                    />
                  </Stack>

                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Term</TableCell>
                        <TableCell align="right">Exposure</TableCell>
                        <TableCell align="right">Current (mA, or mAh per event)</TableCell>
                        <TableCell align="right">mAh/day</TableCell>
                        <TableCell align="right">Share</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {rows.map((r, i) => (
                        <TableRow key={r.key} sx={i === 0 ? { "& td": { fontWeight: "bold" } } : undefined}>
                          <TableCell>{r.label}</TableCell>
                          <TableCell align="right">{fmtExposure(r.exposure, r.unit)}</TableCell>
                          <TableCell align="right">
                            <TextField
                              size="small"
                              type="number"
                              value={r.current}
                              onChange={(e) => {
                                const v = Number(e.target.value);
                                if (isFinite(v) && v >= 0) setEdited(withTermCurrent(model, r.key, v));
                              }}
                              slotProps={{ htmlInput: { step: "any", min: 0, "aria-label": `${r.label} current` } }}
                              sx={{ width: 110 }}
                            />
                          </TableCell>
                          <TableCell align="right">{r.mAhPerDay.toFixed(2)}</TableCell>
                          <TableCell align="right">{(r.share * 100).toFixed(1)}%</TableCell>
                        </TableRow>
                      ))}
                    </TableBody>
                  </Table>
                  <Stack direction="row" spacing={2} sx={{ alignItems: "center" }}>
                    <Button size="small" disabled={!edited} onClick={() => setEdited(null)}>
                      Reset
                    </Button>
                    <Typography variant="caption" color="text.secondary">
                      Edits are not saved. Fitted currents come from the calibration run
                      (docs/BATTERY_STATS_DESIGN.md §4).
                    </Typography>
                  </Stack>
                </>
              )}

              <div>
                <Button size="small" variant="outlined" onClick={download}>
                  Export CSV
                </Button>
              </div>
            </>
          )}
        </Stack>
      </CardContent>
    </Card>
  );
}

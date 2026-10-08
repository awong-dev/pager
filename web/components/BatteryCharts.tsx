"use client";

/** Hand-written SVG charts for the Battery card (docs/BATTERY_STATS_DESIGN.md
 * B7). No chart dependency; colours come from the MUI theme. */

import Box from "@mui/material/Box";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";
import { useTheme, type Theme } from "@mui/material/styles";

import {
  CAUSES,
  GNSS_FLOOR_MV,
  OTA_FLOOR_MV,
  USB_MV,
  type AwakeBucket,
  type BatterySample,
  type Cause,
  floorBucket,
  nextBucket,
} from "@/lib/batteryModel";

const W = 600;
const H = 160;
const PAD = { l: 44, r: 8, t: 8, b: 20 };

function Swatch({ color, label, dashed }: { color: string; label: string; dashed?: boolean }) {
  return (
    <Stack direction="row" spacing={0.5} sx={{ alignItems: "center" }}>
      <Box
        sx={{
          width: 14,
          height: dashed ? 0 : 8,
          bgcolor: dashed ? undefined : color,
          borderTop: dashed ? `2px dashed ${color}` : undefined,
        }}
      />
      <Typography variant="caption">{label}</Typography>
    </Stack>
  );
}

function tickLabel(t: number, spanS: number): string {
  const d = new Date(t * 1000);
  if (spanS <= 2 * 86400) return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  return d.toLocaleDateString([], { month: "short", day: "numeric" });
}

/** Up to 4 x ticks snapped to local midnights (span > 2 days) or whole local
 * hours, always including the first and last boundary in [t0, t1]. Falls back
 * to thirds when fewer than 2 boundaries exist. */
function voltageTicks(t0: number, t1: number): number[] {
  const unit = t1 - t0 > 2 * 86400 ? "day" : "hour";
  const bounds: number[] = [];
  for (let t = floorBucket(t0, unit); t <= t1; t = nextBucket(t, unit)) {
    if (t >= t0) bounds.push(t);
  }
  if (bounds.length < 2) return [0, 1, 2, 3].map((i) => t0 + ((t1 - t0) * i) / 3);
  const count = Math.min(4, bounds.length);
  const idx = new Set<number>();
  for (let i = 0; i < count; i++) idx.add(Math.round((i * (bounds.length - 1)) / (count - 1)));
  return [...idx].sort((a, b) => a - b).map((i) => bounds[i]);
}

export function VoltageChart({ samples }: { samples: BatterySample[] }) {
  const theme = useTheme();
  const pts = samples.filter((s) => s.battMv != null);
  if (pts.length === 0) return null;
  const t0 = pts[0].ts;
  const t1 = Math.max(pts[pts.length - 1].ts, t0 + 1);
  const vals: number[] = [];
  for (const s of pts) {
    vals.push(s.battMv as number);
    if (s.minMv != null) vals.push(s.minMv);
  }
  const lo = Math.max(3000, Math.min(...vals) - 20);
  const hi = Math.min(4500, Math.max(...vals) + 20);
  const x = (t: number) => PAD.l + ((t - t0) / (t1 - t0)) * (W - PAD.l - PAD.r);
  const y = (mv: number) => PAD.t + (1 - (mv - lo) / Math.max(1, hi - lo)) * (H - PAD.t - PAD.b);
  const line = (get: (s: BatterySample) => number | undefined) =>
    pts
      .filter((s) => get(s) != null && (s.battMv as number) < USB_MV)
      .map((s) => `${x(s.ts).toFixed(1)},${y(get(s) as number).toFixed(1)}`)
      .join(" ");
  const usb = pts.filter((s) => (s.battMv as number) >= USB_MV);
  const grey = theme.palette.grey[500];
  const floors = [
    { mv: GNSS_FLOOR_MV, label: "GNSS off below", color: theme.palette.warning.main },
    { mv: OTA_FLOOR_MV, label: "OTA off below", color: theme.palette.info.main },
  ].filter((f) => f.mv >= lo && f.mv <= hi);
  const ticks = voltageTicks(t0, t1);
  return (
    <Box>
      <svg viewBox={`0 0 ${W} ${H}`} width="100%" role="img" aria-label="Battery voltage over time">
        <line x1={PAD.l} x2={PAD.l} y1={PAD.t} y2={H - PAD.b} stroke={theme.palette.divider} />
        <line x1={PAD.l} x2={W - PAD.r} y1={H - PAD.b} y2={H - PAD.b} stroke={theme.palette.divider} />
        {[lo, hi].map((v) => (
          <text key={v} x={PAD.l - 4} y={y(v) + 4} fontSize="10" textAnchor="end" fill={theme.palette.text.secondary}>
            {Math.round(v)}
          </text>
        ))}
        {ticks.map((t, i) => (
          <text
            key={i}
            x={x(t)}
            y={H - 6}
            fontSize="10"
            textAnchor={i === 0 ? "start" : i === ticks.length - 1 ? "end" : "middle"}
            fill={theme.palette.text.secondary}
          >
            {tickLabel(t, t1 - t0)}
          </text>
        ))}
        {floors.map((f) => (
          <g key={f.mv}>
            <line x1={PAD.l} x2={W - PAD.r} y1={y(f.mv)} y2={y(f.mv)} stroke={f.color} strokeDasharray="6 4" />
            <text x={W - PAD.r - 2} y={y(f.mv) - 3} fontSize="9" textAnchor="end" fill={f.color}>
              {f.label} ({f.mv})
            </text>
          </g>
        ))}
        <polyline
          points={line((s) => s.minMv)}
          fill="none"
          stroke={theme.palette.secondary.main}
          strokeWidth="1.5"
          strokeDasharray="2 3"
        />
        <polyline points={line((s) => s.battMv)} fill="none" stroke={theme.palette.primary.main} strokeWidth="1.5" />
        {pts
          .filter((s) => (s.battMv as number) < USB_MV)
          .map((s, i) => (
            <circle key={i} cx={x(s.ts)} cy={y(s.battMv as number)} r="1.5" fill={theme.palette.primary.main} />
          ))}
        {usb.map((s, i) => (
          <circle key={i} cx={x(s.ts)} cy={y(Math.min(hi, s.battMv as number))} r="2" fill={grey} />
        ))}
      </svg>
      <Stack direction="row" spacing={2} sx={{ flexWrap: "wrap" }}>
        <Swatch color={theme.palette.primary.main} label="battMv (at publish)" />
        <Swatch color={theme.palette.secondary.main} label="minMv (window minimum)" dashed />
        {usb.length > 0 && <Swatch color={grey} label="on USB (not battery)" />}
      </Stack>
    </Box>
  );
}

export function causeColors(theme: Theme): Record<Cause, string> {
  return {
    timer: theme.palette.info.light,
    attn: theme.palette.success.main,
    hot: theme.palette.warning.main,
    ui: theme.palette.secondary.main,
    modem: theme.palette.primary.main,
    fetch: theme.palette.error.main,
  };
}

export function AwakeStackChart({ buckets, unitLabel }: { buckets: AwakeBucket[]; unitLabel: string }) {
  const theme = useTheme();
  const colors = causeColors(theme);
  if (buckets.length === 0) return null;
  const totals = buckets.map((b) => CAUSES.reduce((a, c) => a + b.values[c], 0));
  const ymax = Math.max(1, ...totals);
  const plotW = W - PAD.l - PAD.r;
  const bw = plotW / buckets.length;
  const y = (v: number) => PAD.t + (1 - v / ymax) * (H - PAD.t - PAD.b);
  const rangeS = buckets[buckets.length - 1].t1 - buckets[0].t0;
  // Day buckets are always labelled by date, hour buckets by time of day.
  const bucketLabel = (b: AwakeBucket) =>
    b.t1 - b.t0 > 2 * 3600
      ? new Date(b.t0 * 1000).toLocaleDateString([], { month: "short", day: "numeric" })
      : tickLabel(b.t0, rangeS);
  const sums = {} as Record<Cause, number>;
  for (const c of CAUSES) sums[c] = buckets.reduce((a, b) => a + b.values[c], 0);
  return (
    <Box>
      <svg viewBox={`0 0 ${W} ${H}`} width="100%" role="img" aria-label={`Awake seconds ${unitLabel}`}>
        <line x1={PAD.l} x2={PAD.l} y1={PAD.t} y2={H - PAD.b} stroke={theme.palette.divider} />
        <line x1={PAD.l} x2={W - PAD.r} y1={H - PAD.b} y2={H - PAD.b} stroke={theme.palette.divider} />
        <text x={PAD.l - 4} y={PAD.t + 8} fontSize="10" textAnchor="end" fill={theme.palette.text.secondary}>
          {Math.round(ymax)}s
        </text>
        <text x={PAD.l - 4} y={H - PAD.b} fontSize="10" textAnchor="end" fill={theme.palette.text.secondary}>
          0
        </text>
        {[0, buckets.length - 1].map((i) => (
          <text
            key={i}
            x={i === 0 ? PAD.l : W - PAD.r}
            y={H - 6}
            fontSize="10"
            textAnchor={i === 0 ? "start" : "end"}
            fill={theme.palette.text.secondary}
          >
            {bucketLabel(buckets[i])}
          </text>
        ))}
        {buckets.map((b, i) => {
          let acc = 0;
          return (
            <g key={b.t0}>
              {CAUSES.map((c) => {
                const v = b.values[c];
                if (v <= 0) return null;
                const yTop = y(acc + v);
                const yBot = y(acc);
                acc += v;
                return (
                  <rect
                    key={c}
                    x={PAD.l + i * bw + 0.5}
                    y={yTop}
                    width={Math.max(0.5, bw - 1)}
                    height={Math.max(0, yBot - yTop)}
                    fill={colors[c]}
                  />
                );
              })}
            </g>
          );
        })}
      </svg>
      <Typography variant="caption" color="text.secondary" component="div">
        Awake seconds {unitLabel}, stacked by cause
      </Typography>
      <Stack direction="row" spacing={2} sx={{ flexWrap: "wrap" }}>
        {CAUSES.map((c) => (
          <Swatch key={c} color={colors[c]} label={`${c} ${Math.round(sums[c])} s`} />
        ))}
      </Stack>
    </Box>
  );
}

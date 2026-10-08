/** Battery model, pure maths (docs/BATTERY_STATS_DESIGN.md §3). No imports,
 * no enums: `node --experimental-strip-types` runs this file directly. */

export type Cause = "timer" | "attn" | "hot" | "ui" | "modem" | "fetch";
/** Rising priority order, the same as the `aw` array on the wire. */
export const CAUSES: Cause[] = ["timer", "attn", "hot", "ui", "modem", "fetch"];

export const USB_MV = 4300;
export const GNSS_FLOOR_MV = 3550;
export const OTA_FLOOR_MV = 3600;

/** One stored `devices/{id}/battery/*` doc plus its id. Everything that
 * comes from the `bs` sub-map is optional (older firmware sends none). */
export interface BatterySample {
  id?: string;
  ts: number;
  session?: string;
  sq?: number;
  battMv?: number;
  minMv?: number;
  rssi?: number;
  mode?: string;
  xport?: string;
  fw?: string;
  img?: string;
  rst?: string | number;
  link?: string | number;
  hasBs: boolean;
  dtS?: number;
  sleepS?: number;
  awakeS?: Partial<Record<Cause, number>>;
  sleeps?: number;
  ext1?: number;
  railS?: number;
  refresh?: { full?: number; partial?: number; upgraded?: number };
  connects?: number;
  modemS?: { off?: number; search?: number; gnss?: number };
  radioEvents?: number;
}

export interface BatteryModel {
  id?: string;
  sleep_mA: number;
  awake_mA: Record<Cause, number>;
  rail_mA: number;
  modemIdle_mA: number;
  modemOff_mA: number;
  modemSearch_mA: number;
  gnss_mA: number;
  radioEvent_mAh: number;
  connect_mAh: number;
  fullRefresh_mAh: number;
  partialRefresh_mAh: number;
  capacityMah: number;
  usableFrac: number;
  source?: "prior" | "fit";
  fitAt?: number | null;
  notes?: string;
}

const n = (v: number | undefined): number => (typeof v === "number" && isFinite(v) ? v : 0);

interface Exposure {
  awake: Record<Cause, number>;
  sleepS: number;
  railS: number;
  idleS: number;
  offS: number;
  searchS: number;
  gnssS: number;
  radioEvents: number;
  connects: number;
  full: number;
  partial: number;
}

function exposure(s: BatterySample): Exposure {
  const off = n(s.modemS?.off);
  const search = n(s.modemS?.search);
  const gnss = n(s.modemS?.gnss);
  const awake = {} as Record<Cause, number>;
  for (const c of CAUSES) awake[c] = n(s.awakeS?.[c]);
  return {
    awake,
    sleepS: n(s.sleepS),
    railS: n(s.railS),
    idleS: Math.max(0, n(s.dtS) - off - search - gnss),
    offS: off,
    searchS: search,
    gnssS: gnss,
    radioEvents: n(s.radioEvents),
    connects: n(s.connects),
    full: n(s.refresh?.full) + n(s.refresh?.upgraded),
    partial: n(s.refresh?.partial),
  };
}

/** Design §3 formula over one sample; null when it carries no counters. */
export function sampleMah(s: BatterySample, m: BatteryModel): number | null {
  if (!s.hasBs) return null;
  const e = exposure(s);
  let mAs = m.sleep_mA * e.sleepS + m.rail_mA * e.railS + m.modemIdle_mA * e.idleS;
  mAs += m.modemOff_mA * e.offS + m.modemSearch_mA * e.searchS + m.gnss_mA * e.gnssS;
  for (const c of CAUSES) mAs += m.awake_mA[c] * e.awake[c];
  return (
    mAs / 3600 +
    m.radioEvent_mAh * e.radioEvents +
    m.connect_mAh * e.connects +
    m.fullRefresh_mAh * e.full +
    m.partialRefresh_mAh * e.partial
  );
}

export interface TermRow {
  key: string;
  label: string;
  exposure: number;
  unit: "h" | "count";
  current: number;
  mAhPerDay: number;
  share: number;
}

/** Read a term's current from the model by `TermRow.key`. */
export function termCurrent(m: BatteryModel, key: string): number {
  if (key.startsWith("aw_")) return m.awake_mA[key.slice(3) as Cause];
  switch (key) {
    case "sleep": return m.sleep_mA;
    case "rail": return m.rail_mA;
    case "idle": return m.modemIdle_mA;
    case "off": return m.modemOff_mA;
    case "search": return m.modemSearch_mA;
    case "gnss": return m.gnss_mA;
    case "re": return m.radioEvent_mAh;
    case "cn": return m.connect_mAh;
    case "full": return m.fullRefresh_mAh;
    default: return m.partialRefresh_mAh;
  }
}

/** Return a copy of the model with one term's current replaced. */
export function withTermCurrent(m: BatteryModel, key: string, v: number): BatteryModel {
  const c: BatteryModel = { ...m, awake_mA: { ...m.awake_mA } };
  if (key.startsWith("aw_")) c.awake_mA[key.slice(3) as Cause] = v;
  else if (key === "sleep") c.sleep_mA = v;
  else if (key === "rail") c.rail_mA = v;
  else if (key === "idle") c.modemIdle_mA = v;
  else if (key === "off") c.modemOff_mA = v;
  else if (key === "search") c.modemSearch_mA = v;
  else if (key === "gnss") c.gnss_mA = v;
  else if (key === "re") c.radioEvent_mAh = v;
  else if (key === "cn") c.connect_mAh = v;
  else if (key === "full") c.fullRefresh_mAh = v;
  else c.partialRefresh_mAh = v;
  return c;
}

/** One row per model term, largest share first. */
export function termBreakdown(samples: BatterySample[], m: BatteryModel): TermRow[] {
  const bs = samples.filter((s) => s.hasBs);
  const sum: Exposure = {
    awake: { timer: 0, attn: 0, hot: 0, ui: 0, modem: 0, fetch: 0 },
    sleepS: 0, railS: 0, idleS: 0, offS: 0, searchS: 0, gnssS: 0,
    radioEvents: 0, connects: 0, full: 0, partial: 0,
  };
  let dt = 0;
  for (const s of bs) {
    const e = exposure(s);
    dt += n(s.dtS);
    for (const c of CAUSES) sum.awake[c] += e.awake[c];
    sum.sleepS += e.sleepS; sum.railS += e.railS; sum.idleS += e.idleS;
    sum.offS += e.offS; sum.searchS += e.searchS; sum.gnssS += e.gnssS;
    sum.radioEvents += e.radioEvents; sum.connects += e.connects;
    sum.full += e.full; sum.partial += e.partial;
  }
  const defs: { key: string; label: string; unit: "h" | "count"; exp: number }[] = [
    ...CAUSES.map((c) => ({ key: `aw_${c}`, label: `ESP32 awake: ${c}`, unit: "h" as const, exp: sum.awake[c] / 3600 })),
    { key: "sleep", label: "ESP32 light sleep", unit: "h", exp: sum.sleepS / 3600 },
    { key: "rail", label: "3V3 rail (display + keyboard)", unit: "h", exp: sum.railS / 3600 },
    { key: "idle", label: "Modem registered idle", unit: "h", exp: sum.idleS / 3600 },
    { key: "off", label: "Modem off", unit: "h", exp: sum.offS / 3600 },
    { key: "search", label: "Modem searching", unit: "h", exp: sum.searchS / 3600 },
    { key: "gnss", label: "GNSS", unit: "h", exp: sum.gnssS / 3600 },
    { key: "re", label: "Radio events", unit: "count", exp: sum.radioEvents },
    { key: "cn", label: "MQTT reconnects", unit: "count", exp: sum.connects },
    { key: "full", label: "Full refreshes (incl. upgraded)", unit: "count", exp: sum.full },
    { key: "partial", label: "Partial refreshes", unit: "count", exp: sum.partial },
  ];
  const rows = defs.map((d) => {
    const current = termCurrent(m, d.key);
    return { key: d.key, label: d.label, exposure: d.exp, unit: d.unit, current, mAh: d.exp * current };
  });
  const total = rows.reduce((a, r) => a + r.mAh, 0);
  return rows
    .map((r) => ({
      key: r.key,
      label: r.label,
      exposure: r.exposure,
      unit: r.unit,
      current: r.current,
      mAhPerDay: dt > 0 ? (r.mAh * 86400) / dt : 0,
      share: total > 0 ? r.mAh / total : 0,
    }))
    .sort((a, b) => b.share - a.share);
}

export interface AwakeBucket {
  t0: number;
  /** Bucket end (start of the next local hour/day). */
  t1: number;
  values: Record<Cause, number>;
}

export type BucketUnit = "hour" | "day";

/** Start (epoch s) of the viewer's local hour/day containing `tS`. */
export function floorBucket(tS: number, unit: BucketUnit): number {
  const d = new Date(tS * 1000);
  if (unit === "hour") d.setMinutes(0, 0, 0);
  else d.setHours(0, 0, 0, 0);
  return d.getTime() / 1000;
}

/** Start of the local hour/day after the bucket starting at `t0S`. Local
 * days are 23 or 25 h across DST changes. */
export function nextBucket(t0S: number, unit: BucketUnit): number {
  if (unit === "hour") return floorBucket(t0S + 3600, "hour");
  const d = new Date(t0S * 1000);
  d.setDate(d.getDate() + 1);
  d.setHours(0, 0, 0, 0);
  return d.getTime() / 1000;
}

/** Spread each sample's awake seconds evenly over [ts - dtS, ts] into
 * local-clock hour or day buckets. Contiguous from first to last. */
export function bucketAwake(samples: BatterySample[], unit: BucketUnit): AwakeBucket[] {
  const map = new Map<number, AwakeBucket>();
  const get = (t0: number): AwakeBucket => {
    let b = map.get(t0);
    if (!b) {
      b = { t0, t1: nextBucket(t0, unit), values: { timer: 0, attn: 0, hot: 0, ui: 0, modem: 0, fetch: 0 } };
      map.set(t0, b);
    }
    return b;
  };
  for (const s of samples) {
    if (!s.hasBs) continue;
    const dt = n(s.dtS);
    const end = s.ts;
    const start = end - dt;
    if (dt <= 0) {
      const b = get(floorBucket(end, unit));
      for (const c of CAUSES) b.values[c] += n(s.awakeS?.[c]);
      continue;
    }
    for (let t0 = floorBucket(start, unit); t0 < end; t0 = nextBucket(t0, unit)) {
      const overlap = Math.min(end, nextBucket(t0, unit)) - Math.max(start, t0);
      if (overlap <= 0) continue;
      const f = overlap / dt;
      const b = get(t0);
      for (const c of CAUSES) b.values[c] += n(s.awakeS?.[c]) * f;
    }
  }
  const keys = [...map.keys()].sort((a, b) => a - b);
  if (keys.length === 0) return [];
  const out: AwakeBucket[] = [];
  for (let t0 = keys[0]; t0 <= keys[keys.length - 1]; t0 = nextBucket(t0, unit)) out.push(get(t0));
  return out;
}

export const CSV_HEADER =
  "ts,iso,session,sq,battMv,minMv,rssi,mode,xport,fw,img,rst,link,hasBs,dtS,sleepS,aw_timer,aw_attn,aw_hot,aw_ui,aw_modem,aw_fetch,sleeps,ext1,railS,rf_full,rf_partial,rf_upgraded,connects,md_off,md_search,md_gnss,radioEvents,model_mAh";

function cell(v: string | number | boolean | undefined | null): string {
  if (v === undefined || v === null) return "";
  if (typeof v === "boolean") return v ? "1" : "0";
  const s = String(v);
  return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

export function toCsv(samples: BatterySample[], m: BatteryModel): string {
  const lines = [CSV_HEADER];
  for (const s of samples) {
    const mah = sampleMah(s, m);
    const row = [
      s.ts,
      new Date(s.ts * 1000).toISOString(),
      s.session, s.sq, s.battMv, s.minMv, s.rssi, s.mode, s.xport, s.fw, s.img, s.rst, s.link,
      s.hasBs,
      s.dtS, s.sleepS,
      s.awakeS?.timer, s.awakeS?.attn, s.awakeS?.hot, s.awakeS?.ui, s.awakeS?.modem, s.awakeS?.fetch,
      s.sleeps, s.ext1, s.railS,
      s.refresh?.full, s.refresh?.partial, s.refresh?.upgraded,
      s.connects,
      s.modemS?.off, s.modemS?.search, s.modemS?.gnss,
      s.radioEvents,
      mah === null ? undefined : mah.toFixed(4),
    ];
    lines.push(row.map(cell).join(","));
  }
  return lines.join("\n");
}

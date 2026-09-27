/**
 * Pure helpers for rendering a location fix (`LocationFixDoc` /
 * `MessageLocDoc`, `lib/types.ts`) -- kept dependency-free (no Leaflet, no
 * MUI, no `window`) so the zoom/staleness/coarseness rules can be read and
 * reasoned about on their own. `web/` has no test runner installed
 * (checked `package.json`: no vitest/jest, and the brief says not to add
 * one just for this) so these aren't exercised by anything but
 * `LocationMap`/`LocationCard`/`LocationTimeline`/`/location` today; keeping
 * them pure and side-effect free at least makes them easy to hand-check or
 * wire into a runner later.
 */

import type { LocationFixDoc } from "./types";

// A fix older than this reads as "stale" in `LocationCard` -- bold, warning
// colour, an explicit "may be out of date" suffix on the age string. Chosen
// as "old enough a parent should not assume this is where the kid is right
// now", not derived from any relay constant (the backoff schedule in
// `docs/OVERVIEW.md`'s Location section governs when a *new* fix is
// attempted, not when an old one should stop being trusted at a glance).
export const STALE_FIX_MS = 30 * 60 * 1000;

export function isStaleFix(fixTsMs: number, nowMs: number = Date.now()): boolean {
  return nowMs - fixTsMs > STALE_FIX_MS;
}

// An accuracy radius at or above this is too coarse to draw as a precise
// point, regardless of source.
export const COARSE_ACCURACY_M = 500;

/** True if this fix should be drawn/described as approximate: a cell-based
 * fix always is (even lacking `accM`), and so is any fix whose `accM` is at
 * or above `COARSE_ACCURACY_M` regardless of source (e.g. a degraded GNSS
 * fix). Drives both `LocationMap`'s circle-only-vs-pin choice and
 * `LocationCard`'s "approximate" copy. */
export function isCoarseFix(src: string | null | undefined, accM: number | null | undefined): boolean {
  return src === "cell" || (accM != null && accM >= COARSE_ACCURACY_M);
}

const EARTH_CIRCUMFERENCE_M = 40_075_016.686; // WGS84 equatorial circumference
const TILE_SIZE_PX = 256; // Leaflet/OSM's slippy-map tile size
const MIN_ZOOM = 2;
const MAX_ZOOM = 19; // OSM's raster tiles stop serving beyond this
const DEFAULT_ZOOM_NO_ACCURACY = 16; // a plausible "close in" zoom when accuracy is unknown
// How much of the map's shorter side the accuracy circle's diameter should
// occupy: big enough to read as "the fix is somewhere in here", small
// enough to leave surrounding context (roads, the school, home) visible.
const TARGET_CIRCLE_FRACTION = 0.6;

/**
 * Integer zoom level (clamped to what OSM's tiles serve) that frames an
 * accuracy circle of radius `accM` metres around latitude `lat`, using the
 * standard Web Mercator metres-per-pixel formula. A 15 m GNSS fix zooms to
 * street level; a >= 1 km cell-tower fix zooms out until the whole circle
 * fits, which is also what makes it *look* coarse rather than like a
 * precise pin. `viewportPx` is the map's on-screen size in CSS pixels (not
 * layout-critical -- a rough estimate is fine, it only shifts the zoom by
 * at most one level either way).
 */
export function zoomForAccuracy(
  accM: number | null | undefined,
  viewportPx = 260,
  lat = 0
): number {
  if (accM == null || accM <= 0) return DEFAULT_ZOOM_NO_ACCURACY;
  const targetDiameterPx = viewportPx * TARGET_CIRCLE_FRACTION;
  const metersPerPixel = (2 * accM) / targetDiameterPx;
  const z = Math.log2(
    (EARTH_CIRCUMFERENCE_M * Math.cos((lat * Math.PI) / 180)) / (TILE_SIZE_PX * metersPerPixel)
  );
  if (!Number.isFinite(z)) return DEFAULT_ZOOM_NO_ACCURACY;
  return Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, Math.round(z)));
}

/** Shared with `LocationCard` -- factored out so `/location`'s timeline can
 * describe a fix's source with the exact same words as the map card. */
export function sourceLabel(src: string | null | undefined): string {
  return src === "gnss" ? "GPS" : src === "cell" ? "cell tower" : (src ?? "unknown source");
}

// docs/LOCATION_TRACKING_DESIGN.md §5's `why` values (`relay/app/wire.py`'s
// `_WHY_VALUES`), in the pager's own vocabulary -- mapped to the words a
// parent reads in `/location`'s timeline. `reqId` (checked by `whyLabel`
// below, not here) takes priority when set: a fix answering a `/locate`
// call is more usefully described as "requested" than by whatever mode the
// device happened to be in when it answered.
const WHY_LABELS: Record<string, string> = {
  still: "hourly check-in",
  move: "moving",
  stop: "stopped",
  cell: "cell fix",
  gnss: "GPS fix",
};

/** Human label for a timeline entry's `why`/`reqId` -- `null` renders
 * nothing (an older fix predating `why`, or the synthetic "position
 * unknown" row, which has neither). An unrecognised `why` (schema grew a
 * value this file hasn't been taught yet) falls back to the raw string
 * rather than hiding it. */
export function whyLabel(
  why: string | null | undefined,
  reqId: string | null | undefined
): string | null {
  if (reqId) return "requested";
  if (!why) return null;
  return WHY_LABELS[why] ?? why;
}

// The retention *setting* (`settings/retention.locations`,
// `relay/app/store/settings.py`) defaults to 1 week but is admin-editable,
// and the relay's sweep job that actually deletes old fixes runs
// periodically, not instantly -- so a longer setting, or a sweep that
// simply hasn't run yet, could otherwise leave older points sitting in
// Firestore for `/location` to display. The timeline enforces its own,
// independent 7-day window on every read so what a viewer sees never
// silently grows past "the last week" regardless of server-side settings
// or sweep timing.
export const LOCATION_TIMELINE_WINDOW_MS = 7 * 24 * 60 * 60 * 1000;

/** One row of `/location`'s timeline -- either a real stored fix
 * (`positionUnknown: false`, `lat`/`lon`/`src` all set) or the synthetic
 * "cell only, position unknown" row `buildLocationTimeline` synthesises
 * from `devices/{id}.status.lastCell` (see that function's docstring). */
export interface LocationTimelineEntry {
  id: string;
  /** `LocationFixDoc.ts` (or `lastCell.ts`), in ms. The start of the range
   * for a merged dwell doc. */
  startMs: number;
  /** `LocationFixDoc.lastTs`, in ms -- set only when a later report
   * extended this same dwell doc, per that field's own docstring in
   * `lib/types.ts`. `null` renders as a single time, not a range. */
  endMs: number | null;
  lat: number | null;
  lon: number | null;
  accM: number | null;
  src: string | null;
  why: string | null;
  reqId: string | null;
  cached: boolean;
  positionUnknown: boolean;
}

export interface LocationFixRow extends LocationFixDoc {
  id: string;
}

/**
 * Builds `/location`'s timeline rows from a device's `locations` docs plus
 * its denormalised `status.lastCell`, newest first, already windowed to
 * `LOCATION_TIMELINE_WINDOW_MS`.
 *
 * The synthetic "position unknown" row: `relay/app/location.py`'s
 * `ingest_loc` writes a `locations` doc only when a fix -- GNSS or a
 * cellgeo-resolved cell position -- actually resolves to coordinates; an
 * unsolicited report that cellgeo *cannot* resolve, or one skipped
 * entirely because it arrived within the device's 120s same-cell floor,
 * updates `status.lastCell.ts` (unconditionally, per that field's own
 * docstring in `app/store/devices.py`) but writes no `locations` doc at
 * all. So whenever `lastCell.ts` is newer than the newest real fix, the
 * device *has* reported since that fix but the report carried no
 * resolvable position -- shown as one extra row reading "cell only,
 * position unknown", with no coordinates and therefore no map marker.
 */
export function buildLocationTimeline(
  fixes: LocationFixRow[],
  lastCellTsS: number | null | undefined,
  nowMs: number = Date.now()
): LocationTimelineEntry[] {
  const cutoffMs = nowMs - LOCATION_TIMELINE_WINDOW_MS;

  const entries: LocationTimelineEntry[] = [];
  for (const f of fixes) {
    const startMs = f.ts * 1000;
    const endMs = f.lastTs != null ? f.lastTs * 1000 : null;
    if ((endMs ?? startMs) < cutoffMs) continue;
    entries.push({
      id: f.id,
      startMs,
      endMs,
      lat: f.lat,
      lon: f.lon,
      accM: f.accM,
      src: f.src,
      why: f.why ?? null,
      reqId: f.reqId ?? null,
      cached: f.cached,
      positionUnknown: false,
    });
  }
  entries.sort((a, b) => (b.endMs ?? b.startMs) - (a.endMs ?? a.startMs));

  if (lastCellTsS != null) {
    const lastCellMs = lastCellTsS * 1000;
    const newestFixMs = entries[0] ? (entries[0].endMs ?? entries[0].startMs) : 0;
    if (lastCellMs > newestFixMs && lastCellMs >= cutoffMs) {
      entries.unshift({
        id: `lastCell-${lastCellTsS}`,
        startMs: lastCellMs,
        endMs: null,
        lat: null,
        lon: null,
        accM: null,
        src: "cell",
        why: null,
        reqId: null,
        cached: false,
        positionUnknown: true,
      });
    }
  }

  return entries;
}

export interface LocationTimelineDayGroup {
  dayKey: string;
  entries: LocationTimelineEntry[];
}

/** Groups an already newest-first `buildLocationTimeline` result by local
 * calendar day, preserving newest-day-first order (a plain `Map` keeps
 * insertion order, and the input is newest-first already). `dayKey` is the
 * group's own `startMs`, stable enough for a React list key and for
 * `formatDay` (lib/time.ts) to label -- not itself a display string. */
export function groupTimelineByDay(entries: LocationTimelineEntry[]): LocationTimelineDayGroup[] {
  const groups = new Map<string, LocationTimelineEntry[]>();
  for (const e of entries) {
    const d = new Date(e.startMs);
    const key = `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`;
    const list = groups.get(key);
    if (list) list.push(e);
    else groups.set(key, [e]);
  }
  return Array.from(groups.entries()).map(([dayKey, es]) => ({ dayKey, entries: es }));
}

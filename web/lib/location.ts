/**
 * Pure helpers for rendering a location fix (`LocationFixDoc` /
 * `MessageLocDoc`, `lib/types.ts`) -- kept dependency-free (no Leaflet, no
 * MUI, no `window`) so the zoom/staleness/coarseness rules can be read and
 * reasoned about on their own. `web/` has no test runner installed
 * (checked `package.json`: no vitest/jest, and the brief says not to add
 * one just for this) so these aren't exercised by anything but
 * `LocationMap`/`LocationCard` today; keeping them pure and side-effect
 * free at least makes them easy to hand-check or wire into a runner later.
 */

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

"use client";

/**
 * Leaflet map for a single location fix, used by `LocationCard` via
 * `next/dynamic(..., { ssr: false })`. Never import this file directly --
 * Leaflet touches `window`/`document` at module load, and this app is a
 * static export (`output: 'export'`): "use client" pages are still
 * prerendered once at build time with no DOM
 * (node_modules/next/dist/docs/01-app/02-guides/static-exports.md,
 * "Browser APIs"), so an eager import here would crash `next build`.
 * `ssr: false` skips that prerender for this component entirely.
 *
 * No `react-leaflet`: Leaflet's own imperative API is a handful of
 * `useEffect` lines for the one map shape this app needs (a point, an
 * accuracy circle, an optional trail), so a wrapper library buys nothing
 * here and is one more peer-dependency surface to keep aligned with React
 * 19/Next 16 (react-leaflet 5's peer range is `react[-dom]: ^19.0.0`, which
 * does match today, but "fewer moving parts" per the brief still favours
 * not adding it).
 *
 * Default markers use `L.divIcon` (inline HTML, no image assets) instead of
 * `L.Icon.Default`, whose bundler-relative icon URLs are broken by default
 * under webpack/Next and need hand patching -- see
 * https://github.com/Leaflet/Leaflet/issues/4968. `divIcon` sidesteps the
 * whole problem.
 */

import "leaflet/dist/leaflet.css";
import L from "leaflet";
import { useEffect, useRef } from "react";

import { isCoarseFix, zoomForAccuracy } from "@/lib/location";
import { OSM_ATTRIBUTION, OSM_TILE_MAX_ZOOM, OSM_TILE_URL } from "@/lib/mapTiles";

const MAP_HEIGHT_PX = 200;
// Used only to pick a zoom level (see `zoomForAccuracy`), not for layout --
// the container itself is always `width: 100%` of its card.
const VIEWPORT_PX_ESTIMATE = 260;

const PRECISE_COLOR = "#2f5d8a"; // theme.ts primary.main
const TRAIL_COLOR = "#8a5d2f"; // theme.ts secondary.main, doubles as the "coarse" colour

export interface LocationMapTrailPoint {
  lat: number;
  lon: number;
}

export default function LocationMap({
  lat,
  lon,
  accM,
  src,
  trail,
}: {
  lat: number;
  lon: number;
  accM?: number | null;
  src?: string | null;
  trail?: LocationMapTrailPoint[];
}) {
  const containerRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;

    const coarse = isCoarseFix(src, accM);
    const zoom = zoomForAccuracy(accM, VIEWPORT_PX_ESTIMATE, lat);

    const map = L.map(el, {
      center: [lat, lon],
      zoom,
      // Mobile-first (docs/SERVER_PLAN.md §7.7): a scroll/trackpad gesture
      // over the map must not hijack the page. Re-enabled below once the
      // map is actually tapped/focused. Pinch zoom (`touchZoom`) is a
      // separate handler and stays on by default.
      scrollWheelZoom: false,
    });

    const enableScroll = () => map.scrollWheelZoom.enable();
    const disableScroll = () => map.scrollWheelZoom.disable();
    el.addEventListener("click", enableScroll);
    el.addEventListener("focus", enableScroll, true);
    el.addEventListener("mouseleave", disableScroll);
    el.addEventListener("blur", disableScroll, true);

    L.tileLayer(OSM_TILE_URL, {
      maxZoom: OSM_TILE_MAX_ZOOM,
      attribution: OSM_ATTRIBUTION,
    }).addTo(map);

    // Faint trail of recent fixes, oldest to newest, ending at the current
    // point -- only drawn when the caller has more than one point handy.
    if (trail && trail.length > 1) {
      L.polyline(
        trail.map((p): [number, number] => [p.lat, p.lon]),
        { color: TRAIL_COLOR, weight: 2, opacity: 0.35, dashArray: "4 6" }
      ).addTo(map);
      trail.slice(0, -1).forEach((p, i) => {
        L.circleMarker([p.lat, p.lon], {
          radius: 3,
          weight: 0,
          fillColor: TRAIL_COLOR,
          // Older points fade out; the one right before "now" is the most opaque.
          fillOpacity: 0.15 + (0.35 * (i + 1)) / trail.length,
        }).addTo(map);
      });
    }

    if (accM != null && accM > 0) {
      L.circle([lat, lon], {
        radius: accM,
        color: coarse ? TRAIL_COLOR : PRECISE_COLOR,
        weight: 1,
        fillColor: coarse ? TRAIL_COLOR : PRECISE_COLOR,
        fillOpacity: coarse ? 0.25 : 0.15,
      }).addTo(map);
    }

    if (coarse) {
      // A cell-based (or otherwise very imprecise) fix: no discrete pin --
      // a pin would claim a precision this fix doesn't have. Just a small
      // muted dot at the circle's centre, non-interactive, so the map has
      // something to centre on without reading as "the kid is exactly
      // here".
      L.marker([lat, lon], {
        icon: L.divIcon({
          className: "pager-location-marker pager-location-marker--coarse",
          html: `<span style="display:block;width:8px;height:8px;border-radius:50%;background:${TRAIL_COLOR};opacity:0.6"></span>`,
          iconSize: [8, 8],
          iconAnchor: [4, 4],
        }),
        keyboard: false,
        interactive: false,
      }).addTo(map);
    } else {
      L.marker([lat, lon], {
        icon: L.divIcon({
          className: "pager-location-marker",
          html: `<span style="display:block;width:14px;height:14px;border-radius:50%;background:${PRECISE_COLOR};border:2px solid #fff;box-shadow:0 0 2px rgba(0,0,0,0.6)"></span>`,
          iconSize: [14, 14],
          iconAnchor: [7, 7],
        }),
        keyboard: false,
      }).addTo(map);
    }

    // The container's real size isn't known at construction time (MUI/flex
    // layout hasn't settled yet), so Leaflet's cached size can be stale on
    // first paint -- ask it to recheck once mounted and on resize.
    const invalidate = () => map.invalidateSize();
    const raf = requestAnimationFrame(invalidate);
    window.addEventListener("resize", invalidate);

    return () => {
      cancelAnimationFrame(raf);
      window.removeEventListener("resize", invalidate);
      el.removeEventListener("click", enableScroll);
      el.removeEventListener("focus", enableScroll, true);
      el.removeEventListener("mouseleave", disableScroll);
      el.removeEventListener("blur", disableScroll, true);
      map.remove();
    };
    // Rebuilding the whole map on any change is simple and cheap at this
    // scale (redrawn a handful of times an hour, at most, per open thread).
  }, [lat, lon, accM, src, trail]);

  return (
    <div
      ref={containerRef}
      aria-label="Map of the pager's last known location"
      style={{ height: MAP_HEIGHT_PX, width: "100%", borderRadius: 8, outline: "none" }}
    />
  );
}

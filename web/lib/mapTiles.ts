/**
 * Tile source + attribution for `LocationMap`, kept in one place so a
 * different free provider (e.g. OpenFreeMap/MapLibre) can be swapped in
 * later without touching the map component itself.
 *
 * Decision: Leaflet + OpenStreetMap's own raster tiles. Free, no API key,
 * no account -- fine for one family's traffic under
 * https://operations.osmfoundation.org/policies/tiles/, which in return
 * asks for (a) visible attribution linking to
 * https://www.openstreetmap.org/copyright (below, wired into Leaflet's
 * attribution control), (b) no bulk prefetching (this app only ever loads
 * the tiles for whatever view a person actually opens), and (c) letting
 * the browser's own HTTP cache hold them (no custom caching layer here).
 */
export const OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
export const OSM_TILE_MAX_ZOOM = 19;
export const OSM_ATTRIBUTION =
  '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener noreferrer">OpenStreetMap</a> contributors';

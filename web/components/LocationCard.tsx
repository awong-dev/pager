"use client";

import dynamic from "next/dynamic";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Link from "@mui/material/Link";
import Skeleton from "@mui/material/Skeleton";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";
import RoomIcon from "@mui/icons-material/Room";

import { formatRelativeAge } from "@/lib/time";
import { isCoarseFix, isStaleFix } from "@/lib/location";
import type { LocationMapTrailPoint } from "@/components/LocationMap";

// Leaflet touches `window` at import time -- loaded client-side only. See
// `LocationMap`'s own docstring for why (static export prerenders "use
// client" pages once at build time, with no DOM).
const LocationMap = dynamic(() => import("@/components/LocationMap"), {
  ssr: false,
  loading: () => <Skeleton variant="rectangular" height={200} sx={{ borderRadius: "8px" }} />,
});

/** A location fix rendered as a small OpenStreetMap/Leaflet map (marker +
 * accuracy circle + optional trail) plus lat/lon, accuracy, age, source and
 * "open in maps" links -- docs/SERVER_PLAN.md §7.7. All of the latter stay
 * as plain text below the map so the fix is fully readable without it
 * (screen readers, or the map tile fetch failing/being slow). */
export default function LocationCard({
  lat,
  lon,
  accM,
  fixTsMs,
  src,
  cached,
  trail,
  title = "Last known location",
}: {
  lat: number;
  lon: number;
  accM?: number | null;
  fixTsMs: number;
  /** `"gnss" | "cell"` per docs/SERVER_PLAN.md §3, but typed loosely here
   * (like `lib/types.ts` does) since older data or a future source value
   * should still render, just without special-casing. */
  src?: string | null;
  /** Only `LocationFixDoc` (the device's live location feed) carries this;
   * a message's embedded `loc` (`MessageLocDoc`) does not, so it's optional
   * here and simply omitted from the card when unknown. */
  cached?: boolean | null;
  /** Recent fixes, oldest first, drawn as a faint trail ending at this
   * point. Only available from the device's own `locations` listener
   * (`ThreadPageClient`'s "last known location" card) -- a single shared
   * `loc` message has no history to show. */
  trail?: LocationMapTrailPoint[];
  title?: string;
}) {
  const coords = `${lat.toFixed(5)},${lon.toFixed(5)}`;
  const googleUrl = `https://www.google.com/maps/search/?api=1&query=${coords}`;
  const appleUrl = `https://maps.apple.com/?q=${coords}`;
  const coarse = isCoarseFix(src, accM);
  const stale = isStaleFix(fixTsMs);
  const sourceLabel = src === "gnss" ? "GPS" : src === "cell" ? "cell tower" : (src ?? "unknown source");

  return (
    <Card variant="outlined" sx={{ maxWidth: 360 }}>
      <CardContent>
        <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
          <RoomIcon color="action" fontSize="small" />
          <Typography variant="subtitle2">{title}</Typography>
        </Stack>

        <Stack sx={{ mt: 1, mb: 1 }}>
          <LocationMap
            lat={lat}
            lon={lon}
            accM={accM}
            src={src}
            trail={trail && trail.length > 1 ? trail : undefined}
          />
        </Stack>

        <Typography variant="body2" sx={{ mt: 0.5 }}>
          {lat.toFixed(5)}, {lon.toFixed(5)}
          {accM != null ? ` (±${accM} m)` : ""}
        </Typography>
        {coarse && (
          <Typography variant="caption" color="text.secondary" sx={{ display: "block" }}>
            {src === "cell" ? "Approximate, from the cell tower." : "Approximate -- low-accuracy fix."}
          </Typography>
        )}
        <Typography
          variant="caption"
          color={stale ? "warning.main" : "text.secondary"}
          sx={{ display: "block", fontWeight: stale ? 600 : 400 }}
        >
          {formatRelativeAge(fixTsMs)}
          {stale ? " -- may be out of date" : ""} &middot; {sourceLabel}
          {cached ? " (cached)" : ""}
        </Typography>

        <Stack direction="row" spacing={2} sx={{ mt: 1 }}>
          <Link href={googleUrl} target="_blank" rel="noopener noreferrer">
            Open in Google Maps
          </Link>
          <Link href={appleUrl} target="_blank" rel="noopener noreferrer">
            Open in Apple Maps
          </Link>
        </Stack>
      </CardContent>
    </Card>
  );
}

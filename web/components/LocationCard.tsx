import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Link from "@mui/material/Link";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";
import RoomIcon from "@mui/icons-material/Room";

import { formatRelativeAge } from "@/lib/time";

/** A location fix rendered as lat/lon + accuracy + age + "open in maps"
 * links -- docs/SERVER_PLAN.md §7.7: lat/lon, accuracy, age and an
 * "Open in Google Maps / Apple Maps" link. No embedded map. */
export default function LocationCard({
  lat,
  lon,
  accM,
  fixTsMs,
  title = "Last known location",
}: {
  lat: number;
  lon: number;
  accM?: number | null;
  fixTsMs: number;
  title?: string;
}) {
  const coords = `${lat.toFixed(5)},${lon.toFixed(5)}`;
  const googleUrl = `https://www.google.com/maps/search/?api=1&query=${coords}`;
  const appleUrl = `https://maps.apple.com/?q=${coords}`;

  return (
    <Card variant="outlined" sx={{ maxWidth: 360 }}>
      <CardContent>
        <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
          <RoomIcon color="action" fontSize="small" />
          <Typography variant="subtitle2">{title}</Typography>
        </Stack>
        <Typography variant="body2" sx={{ mt: 0.5 }}>
          {lat.toFixed(5)}, {lon.toFixed(5)}
          {accM != null ? ` (±${accM} m)` : ""}
        </Typography>
        <Typography variant="caption" color="text.secondary">
          {formatRelativeAge(fixTsMs)}
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

"use client";

/** `/location`'s left-hand (or, on a phone, stacked-under-the-map)
 * timeline -- newest first, grouped by local day, each row showing a time
 * (or a range for a merged dwell doc), source + accuracy, and a `why`
 * label. Clicking a row selects it (the parent pans/re-centres the map,
 * `LocationTimelineEntry.lat`/`lon` permitting -- the synthetic "position
 * unknown" row has neither and is still selectable, it just can't move the
 * map). Pure presentation: all the grouping/windowing is
 * `lib/location.ts`'s `buildLocationTimeline`/`groupTimelineByDay`, so this
 * component only ever renders whatever it's handed.
 */

import Box from "@mui/material/Box";
import List from "@mui/material/List";
import ListItemButton from "@mui/material/ListItemButton";
import ListItemText from "@mui/material/ListItemText";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import { groupTimelineByDay, isCoarseFix, sourceLabel, whyLabel, type LocationTimelineEntry } from "@/lib/location";
import { formatClock, formatDay } from "@/lib/time";

function entryTimeLabel(entry: LocationTimelineEntry): string {
  const start = formatClock(entry.startMs);
  if (entry.endMs == null) return start;
  return `${start} – ${formatClock(entry.endMs)}`;
}

function EntryRow({
  entry,
  selected,
  onSelect,
}: {
  entry: LocationTimelineEntry;
  selected: boolean;
  onSelect: (entry: LocationTimelineEntry) => void;
}) {
  const why = whyLabel(entry.why, entry.reqId);
  const coarse = !entry.positionUnknown && isCoarseFix(entry.src, entry.accM);

  return (
    <ListItemButton selected={selected} onClick={() => onSelect(entry)} sx={{ borderRadius: 1 }}>
      <ListItemText
        primary={
          <Stack direction="row" spacing={1} sx={{ alignItems: "baseline" }}>
            <Typography variant="body2" sx={{ fontWeight: 600 }}>
              {entryTimeLabel(entry)}
            </Typography>
            {entry.cached && (
              <Typography variant="caption" color="text.secondary">
                (cached)
              </Typography>
            )}
          </Stack>
        }
        secondary={
          entry.positionUnknown ? (
            <Typography variant="caption" color="text.secondary" sx={{ fontStyle: "italic" }}>
              cell only, position unknown
            </Typography>
          ) : (
            <Typography variant="caption" color="text.secondary">
              {sourceLabel(entry.src)}
              {entry.accM != null ? ` · ±${entry.accM} m` : ""}
              {coarse ? " · approximate" : ""}
              {why ? ` · ${why}` : ""}
            </Typography>
          )
        }
      />
    </ListItemButton>
  );
}

export default function LocationTimeline({
  entries,
  selectedId,
  onSelect,
}: {
  entries: LocationTimelineEntry[];
  selectedId: string | null;
  onSelect: (entry: LocationTimelineEntry) => void;
}) {
  const groups = groupTimelineByDay(entries);

  if (entries.length === 0) {
    return (
      <Typography variant="body2" color="text.secondary">
        No location fixes in the last 7 days.
      </Typography>
    );
  }

  return (
    <Box sx={{ overflowY: "auto", height: "100%" }}>
      {groups.map((g) => (
        <Box key={g.dayKey} sx={{ mb: 1 }}>
          <Typography
            variant="overline"
            color="text.secondary"
            sx={{ display: "block", px: 2, pt: 1 }}
          >
            {formatDay(g.entries[0]!.startMs)}
          </Typography>
          <List dense disablePadding>
            {g.entries.map((entry) => (
              <EntryRow
                key={entry.id}
                entry={entry}
                selected={entry.id === selectedId}
                onSelect={onSelect}
              />
            ))}
          </List>
        </Box>
      ))}
    </Box>
  );
}

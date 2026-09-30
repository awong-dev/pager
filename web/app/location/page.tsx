"use client";

/** `/location` -- docs/SERVER_PLAN.md §7 (this task, 27 Sep 2026 owner
 * request): a standalone location view, split out of the chat thread.
 *
 * Authorization is entirely server-side, unchanged by this page:
 * `firestore.rules`' `devices/{d}/locations/{l}` rule admits the device's
 * owner (listed here as "You", 27 Sep 2026) and `devices/{d}.locatableBy`,
 * which `relay/app/store/allow.py` recomputes
 * from the admin-managed `allow/{fromUid}_{toUid}.locate` edge
 * (`/admin/allowlist`) -- a non-admin can never write `allow` themselves
 * (`app/routers/admin.py`'s allow-list route requires the `admin` claim;
 * every other client write to Firestore is refused outright by this file's
 * closing `allow write: if false`). This page and `lib/locatableDevices.ts`
 * only *read* that denormalised field; they grant nothing.
 *
 * No route param: unlike `/chat/[alias]`/`/devices/[id]`, this page picks
 * its subject from an in-page list rather than the URL, so it needs none of
 * those routes' static-export placeholder-shell machinery. A deep link
 * still works via a plain `?who=<alias>` query string (read once from
 * `window.location.search` via a lazy `useState` initializer, not
 * `useSearchParams` -- avoids that hook's Suspense-boundary requirement
 * under `output: 'export'` for a single optional parameter no other page in
 * this app bothers with either).
 *
 * Selection state is deliberately *derived*, not effect-driven -- both
 * "which person is selected" and (in `LocationDetail` below) "which
 * timeline entry is focused" are plain values computed during render from
 * an explicit user pick (if any) falling back to an automatic default,
 * rather than a `useEffect` that calls `setState` to "adjust" a previous
 * pick. React's own guidance
 * (react.dev/learn/you-might-not-need-an-effect) and this project's lint
 * config (`react-hooks/set-state-in-effect`) both flag the effect form.
 * `LocationDetail` is remounted with `key={device.id}` on every selection
 * change instead of an explicit "reset per-device state" effect, the same
 * trick `chat/[alias]/ThreadPageClient.tsx` uses for its own per-alias
 * state.
 *
 * Layout: a "people I can locate" list, then -- once one is picked -- a
 * timeline (left) and a map (right); on a phone the timeline stacks under
 * the map instead, and the people list is replaced by a back arrow so only
 * one thing is on screen at a time. All reads are Firestore listeners; the
 * one write ("Locate now") goes through `POST
 * /conversations/{alias}/locate`, moved here verbatim from
 * `chat/[alias]/ThreadPageClient.tsx`.
 */

import { collection, limit, onSnapshot, orderBy, query } from "firebase/firestore";
import dynamic from "next/dynamic";
import { useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import List from "@mui/material/List";
import ListItemButton from "@mui/material/ListItemButton";
import ListItemText from "@mui/material/ListItemText";
import Skeleton from "@mui/material/Skeleton";
import Snackbar from "@mui/material/Snackbar";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";
import useMediaQuery from "@mui/material/useMediaQuery";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import LocationOnIcon from "@mui/icons-material/LocationOn";

import AppShell from "@/components/AppShell";
import LocationTimeline from "@/components/LocationTimeline";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { locBackoffLabel } from "@/lib/deviceTrust";
import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { getFirestoreDb } from "@/lib/firebase";
import { buildLocationTimeline, type LocationFixRow } from "@/lib/location";
import { useLocatableDevices, type LocatableDevice } from "@/lib/locatableDevices";
import { formatRelativeAge, tsToMillis } from "@/lib/time";
import type { LocationFixDoc } from "@/lib/types";

const MAP_HEIGHT_PX = 420;
const MOBILE_TIMELINE_HEIGHT_PX = 280;
// Generous headroom over a week's worth of fixes (hourly-while-still is
// ~168/week; moving-every-10-min in the worst case is ~1000/week,
// docs/LOCATION_TRACKING_DESIGN.md §5) rather than a `where('ts', '>=',
// cutoff)` filter -- Firestore requires an inequality-filtered field to be
// the *first* orderBy, which would mean ordering by `ts` instead of
// `createdAt` and, worse, a composite index just for this one query. A
// plain `orderBy(createdAt desc).limit(N)` needs neither; the real 7-day
// window is enforced client-side by `buildLocationTimeline` regardless (see
// `LOCATION_TIMELINE_WINDOW_MS`'s own comment for why that independent
// enforcement matters).
const LOCATIONS_QUERY_LIMIT = 1500;

const LocationMap = dynamic(() => import("@/components/LocationMap"), {
  ssr: false,
  loading: () => <Skeleton variant="rectangular" height={MAP_HEIGHT_PX} sx={{ borderRadius: 1 }} />,
});

// `window.location.search` is unavailable during the static-export
// prerender pass (no DOM); `typeof window` guards that the same way every
// other browser-only read in this app does.
function readWhoParam(): string | null {
  if (typeof window === "undefined") return null;
  return new URLSearchParams(window.location.search).get("who");
}

function PersonListItem({
  device,
  alias,
  selected,
  onSelect,
}: {
  device: LocatableDevice;
  alias: string | undefined;
  selected: boolean;
  onSelect: () => void;
}) {
  const updatedMs = tsToMillis(device.status.updatedAt);
  return (
    <ListItemButton selected={selected} onClick={onSelect} sx={{ borderRadius: 1 }}>
      <ListItemText
        primary={device.isOwn ? "You" : alias ? `@${alias}` : `uid:${device.ownerUid.slice(0, 8)}`}
        secondary={`${device.label}${updatedMs !== null ? ` · updated ${formatRelativeAge(updatedMs)}` : ""}`}
      />
    </ListItemButton>
  );
}

/** The timeline + map for one already-selected device -- given `key={device.id}`
 * by its caller, so switching people remounts this fresh (its own
 * `fixRows`/focused-entry state resets for free, no reset effect needed). */
function LocationDetail({
  device,
  ownerAlias,
  isMobile,
  onBack,
}: {
  device: LocatableDevice;
  ownerAlias: string | undefined;
  isMobile: boolean;
  onBack: () => void;
}) {
  const [fixRows, setFixRows] = useState<LocationFixRow[]>([]);
  const [manualFocusId, setManualFocusId] = useState<string | null>(null);
  const [locateBusy, setLocateBusy] = useState(false);
  const [snack, setSnack] = useState<string | null>(null);

  // A genuine "subscribe to an external system" effect, not the
  // "adjust state" kind this file otherwise avoids (module docstring):
  // `setFixRows` runs inside `onSnapshot`'s own callback, invoked
  // asynchronously whenever Firestore delivers a new snapshot, never
  // synchronously from the effect body itself. This component is remounted
  // (`key={device.id}`, see the caller) on every selection change, so there
  // is no "reset for the new device" case to also handle here.
  useEffect(() => {
    const db = getFirestoreDb();
    const q = query(
      collection(db, "devices", device.id, "locations"),
      orderBy("createdAt", "desc"),
      limit(LOCATIONS_QUERY_LIMIT)
    );
    const unsubscribe = onSnapshot(q, (snap) => {
      setFixRows(snap.docs.map((d) => ({ id: d.id, ...(d.data() as LocationFixDoc) })));
    });
    return unsubscribe;
  }, [device.id]);

  const entries = useMemo(
    () => buildLocationTimeline(fixRows, device.status.lastCell?.ts ?? null),
    [fixRows, device]
  );

  // Focus = whatever the viewer clicked, falling back to the newest entry
  // that actually has a position (the synthetic "position unknown" row,
  // being newest but coordinate-less, must never become the default).
  // Derived, not effect-set -- see module docstring.
  const autoFocusId = useMemo(() => entries.find((e) => e.lat != null)?.id ?? null, [entries]);
  const focusedId = manualFocusId ?? autoFocusId;
  const focusedEntry = useMemo(() => entries.find((e) => e.id === focusedId) ?? null, [entries, focusedId]);

  // Oldest-to-newest, coordinates only -- `LocationMap`'s faint trail.
  const trail = useMemo(
    () =>
      entries
        .filter((e) => e.lat != null && e.lon != null)
        .map((e) => ({ lat: e.lat as number, lon: e.lon as number }))
        .reverse(),
    [entries]
  );

  const backoffLabel = locBackoffLabel(device.status.locBackoffS);

  // docs/SERVER_PLAN.md §5.1/§5.6 -- moved verbatim from
  // `ThreadPageClient.tsx`'s `handleLocate`, just keyed off this page's
  // selected device instead of the chat thread's peer.
  async function handleLocate() {
    if (!ownerAlias) return;
    setLocateBusy(true);
    try {
      const resp = await api.post<{ requestId: string | null; cached: boolean }>(
        `/conversations/${encodeURIComponent(ownerAlias)}/locate`
      );
      setSnack(
        resp.cached
          ? "Answered from a recent fix -- see the timeline."
          : "Location requested -- watch the timeline for the reply."
      );
    } catch (err) {
      setSnack(
        err instanceof ApiError ? String(err.detail ?? "Location request failed.") : "Location request failed."
      );
    } finally {
      setLocateBusy(false);
    }
  }

  const mapArea =
    entries.length === 0 ? (
      <Alert severity="info">No location fixes in the last 7 days.</Alert>
    ) : focusedEntry && focusedEntry.lat != null && focusedEntry.lon != null ? (
      <LocationMap
        lat={focusedEntry.lat}
        lon={focusedEntry.lon}
        accM={focusedEntry.accM}
        src={focusedEntry.src}
        trail={trail.length > 1 ? trail : undefined}
        height={MAP_HEIGHT_PX}
      />
    ) : (
      <Alert severity="info">
        No map position yet -- the latest report was cell-only with no resolvable position.
      </Alert>
    );

  return (
    <Stack spacing={2} sx={{ flexGrow: 1, minWidth: 0 }}>
      <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
        {isMobile && (
          <IconButton onClick={onBack} aria-label="back to people list">
            <ArrowBackIcon />
          </IconButton>
        )}
        <Typography variant="h6">
          {device.isOwn ? `You · ${device.label}` : ownerAlias ? `@${ownerAlias}` : device.label}
        </Typography>
        <Box sx={{ flexGrow: 1 }} />
        <Stack spacing={0.25} sx={{ alignItems: "flex-end" }}>
          {/* docs/FAMILIES_DESIGN.md §5.3: hidden (not just disabled) for a
           * super who neither owns this device nor holds a `locate` edge to
           * it -- `device.canLocate` is `lib/locatableDevices.ts`'s call. */}
          {device.canLocate && (
            <Button
              size="small"
              variant="outlined"
              startIcon={<LocationOnIcon />}
              disabled={locateBusy || !ownerAlias}
              onClick={() => void handleLocate()}
            >
              Locate now
            </Button>
          )}
          {backoffLabel && (
            <Typography variant="caption" color="text.secondary">
              {backoffLabel}
            </Typography>
          )}
          {device.canLocate && !ownerAlias && (
            <Typography variant="caption" color="text.secondary">
              can&apos;t request yet -- alias unknown
            </Typography>
          )}
        </Stack>
      </Stack>

      <Stack direction={isMobile ? "column" : "row"} spacing={2} sx={{ flexGrow: 1, minHeight: 0 }}>
        <Box
          sx={{
            width: isMobile ? "100%" : 320,
            flexShrink: 0,
            height: isMobile ? MOBILE_TIMELINE_HEIGHT_PX : MAP_HEIGHT_PX,
            // Mobile: map first, timeline stacked below it (brief §4).
            order: isMobile ? 2 : 0,
          }}
        >
          <LocationTimeline entries={entries} selectedId={focusedId} onSelect={(e) => setManualFocusId(e.id)} />
        </Box>
        <Box sx={{ flexGrow: 1, minWidth: 0, order: 1 }}>{mapArea}</Box>
      </Stack>

      <Snackbar open={snack !== null} autoHideDuration={5000} onClose={() => setSnack(null)} message={snack} />
    </Stack>
  );
}

function LocationInner() {
  const { uidToAlias } = useDirectory();
  const { me } = useAuth();
  // Own devices: `/locate` is addressed by the owner's alias, which is the
  // viewer's own -- known from `me` even if the directory hasn't learned it.
  const aliasFor = (d: LocatableDevice) => (d.isOwn ? me?.alias : uidToAlias(d.ownerUid));
  const { devices, loaded } = useLocatableDevices();
  // MUI's default `md` breakpoint (900px) -- a raw media-query string, not
  // `useTheme().breakpoints`, matching `ThreadPageClient.tsx`'s existing
  // `useMediaQuery("(prefers-reduced-motion: reduce)")` style rather than
  // introducing this file's only `useTheme` import for one query.
  const isMobile = useMediaQuery("(max-width:899.95px)");

  // One-shot deep-link value -- read once via a lazy initializer, never
  // re-read after mount (the URL itself is never rewritten to reflect the
  // in-page selection, so re-reading it would be pointless).
  const [initialWho] = useState<string | null>(readWhoParam);

  // `undefined` = "no explicit pick yet, use the deep link if it resolves";
  // `null` = "explicitly cleared" (the mobile back arrow) -- once cleared,
  // the deep link must not keep winning on every re-render.
  const [manualSelectedId, setManualSelectedId] = useState<string | null | undefined>(undefined);
  const autoSelectedId = useMemo(() => {
    if (!initialWho) return null;
    // Best-effort, same resolution gap `lib/directory.tsx` documents for
    // everything else: a non-admin's directory only knows aliases it has
    // learned by messaging or localStorage, so this can silently miss a
    // peer never messaged before -- the person is still in the list below,
    // just not pre-selected.
    return devices.find((d) => !d.isOwn && uidToAlias(d.ownerUid) === initialWho)?.id ?? null;
  }, [initialWho, devices, uidToAlias]);
  const selectedId = manualSelectedId !== undefined ? manualSelectedId : autoSelectedId;

  const selectedDevice = useMemo(
    () => devices.find((d) => d.id === selectedId) ?? null,
    [devices, selectedId]
  );
  const ownerAlias = selectedDevice ? aliasFor(selectedDevice) : undefined;

  const peopleList = (
    <Card variant="outlined">
      <CardContent>
        <Typography variant="subtitle1" sx={{ mb: 1 }}>
          People you can locate
        </Typography>
        <List dense disablePadding>
          {devices.map((d) => (
            <PersonListItem
              key={d.id}
              device={d}
              alias={aliasFor(d)}
              selected={d.id === selectedId}
              onSelect={() => setManualSelectedId(d.id)}
            />
          ))}
        </List>
      </CardContent>
    </Card>
  );

  return (
    <Stack spacing={2}>
      <Typography variant="h5">Location</Typography>

      {!loaded && <CircularProgress />}

      {loaded && devices.length === 0 && (
        <Alert severity="info">
          You have no device of your own and no one has authorized you to see their location yet. Ask your
          admin to grant it from the allow-list (&quot;Locate&quot; column).
        </Alert>
      )}

      {loaded &&
        devices.length > 0 &&
        (isMobile ? (
          selectedDevice ? (
            <LocationDetail
              key={selectedDevice.id}
              device={selectedDevice}
              ownerAlias={ownerAlias}
              isMobile={isMobile}
              onBack={() => setManualSelectedId(null)}
            />
          ) : (
            peopleList
          )
        ) : (
          <Stack direction="row" spacing={2} sx={{ alignItems: "flex-start" }}>
            <Box sx={{ width: 280, flexShrink: 0 }}>{peopleList}</Box>
            {selectedDevice ? (
              <LocationDetail
                key={selectedDevice.id}
                device={selectedDevice}
                ownerAlias={ownerAlias}
                isMobile={isMobile}
                onBack={() => setManualSelectedId(null)}
              />
            ) : (
              <Alert severity="info" sx={{ flexGrow: 1 }}>
                Select someone on the left to see their location.
              </Alert>
            )}
          </Stack>
        ))}
    </Stack>
  );
}

export default function LocationPage() {
  return (
    <RequireAuth>
      <AppShell>
        <LocationInner />
      </AppShell>
    </RequireAuth>
  );
}

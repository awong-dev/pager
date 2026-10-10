"use client";

/** `/devices/[id]` -- the battery card (docs/BATTERY_STATS_DESIGN.md B7) and docs/V02_DESIGN.md §6 (device-direct SMS): a contacts
 * editor and an audit log, reachable from `/admin/devices` (any device, via
 * the "SMS" button on each row) and from `/settings/devices` (an owner's
 * own). Authz is the relay's job (device owner or admin, 403 otherwise);
 * this page just calls the API and shows whatever it says.
 *
 * Static-export dynamic segment: like `/chat/[alias]`, this route is
 * exported as a single placeholder shell (`page.tsx`'s
 * `generateStaticParams`/`dynamicParams`) because `output: 'export'` cannot
 * pre-render one HTML file per arbitrary future device id. The id is read
 * from `usePathname()`, not Next's route params, and Firebase Hosting
 * rewrites every real `/devices/<id>` request onto that shell
 * (`web/firebase.json`'s `/devices/** -> /devices/_.html`, mirrored for
 * `next dev` in `next.config.ts`). See
 * `app/chat/[alias]/ThreadPageClient.tsx`'s module docstring for the full
 * rationale -- this route copies it exactly.
 *
 * Relay contract this was built against (docs/V02_DESIGN.md §6; the relay
 * side of this was being built in parallel on this branch, so treat the
 * exact response shapes below as the contract to reconcile against, not a
 * confirmed observation):
 *   GET  /api/devices/{id}/sms-contacts -> {"contacts":[{"name","phone"}], "pending": boolean}
 *   GET  /api/devices/{id}/sms-log?limit=100&before=<epoch s> -> {"entries":[...]} newest first
 * The device header (label, status, `smsLost`) comes from a direct
 * Firestore listener on `devices/{id}` instead (allowed for the owner or an
 * admin by `firestore.rules`), matching this app's "reads are Firestore
 * listeners" convention -- only the SMS contacts/log, which need
 * cursor pagination that a plain `onSnapshot` doesn't give you, go through
 * the API.
 *
 * docs/FAMILIES_DESIGN.md §5.4 / docs/FAMILIES_TASKS.md 3.4: the contacts
 * list is now derived server-side from the owner's approved numbers
 * (`ApprovedEditor` on `/family/people`), so this page only displays it --
 * `PUT /api/devices/{id}/sms-contacts` returns 405 as of relay task 3.2.
 */

import { doc, onSnapshot } from "firebase/firestore";
import { usePathname, useRouter } from "next/navigation";
import { useCallback, useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableContainer from "@mui/material/TableContainer";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import Typography from "@mui/material/Typography";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";

import AppShell from "@/components/AppShell";
import DeviceTrustChip from "@/components/DeviceTrustChip";
import FirmwareChip from "@/components/FirmwareChip";
import FirmwareUpdateDialog from "@/components/FirmwareUpdateDialog";
import RequireAuth from "@/components/RequireAuth";
import BatteryCard from "@/components/BatteryCard";
import WifiPanel from "@/components/WifiPanel";
import GnssPanel from "@/components/GnssPanel";
import { ApiError, api } from "@/lib/api";
import { useAuth } from "@/lib/auth-context";
import { useNewestBuild } from "@/lib/firmware";
import { useDirectory } from "@/lib/directory";
import { xportChipInfo } from "@/lib/deviceTrust";
import { getFirestoreDb } from "@/lib/firebase";
import {
  type SmsContact,
  type SmsContactsResponse,
  type SmsLogEntry,
  type SmsLogResponse,
  dirArrow,
  statusColor,
} from "@/lib/smsContacts";
import type { DeviceDoc } from "@/lib/types";

const LOG_PAGE_SIZE = 100;

function useDeviceId(): string {
  const pathname = usePathname();
  return useMemo(() => {
    const segments = (pathname ?? "").split("/").filter(Boolean);
    return decodeURIComponent(segments[segments.length - 1] ?? "");
  }, [pathname]);
}

function formatLogTs(epochS: number): string {
  return new Date(epochS * 1000).toLocaleString(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });
}

function LogRow({ e }: { e: SmsLogEntry }) {
  const [showRaw, setShowRaw] = useState(false);
  const bad = e.malformed === true;
  return (
    <TableRow
      sx={{
        bgcolor: bad
          ? "warning.light"
          : e.st === "blocked"
            ? "warning.light"
            : e.st === "failed"
              ? "error.light"
              : undefined,
      }}
    >
      <TableCell>{formatLogTs(e.smsTs || e.ts)}</TableCell>
      <TableCell>{e.dir ? dirArrow(e.dir) : "—"}</TableCell>
      <TableCell>{e.name ?? e.peer ?? "—"}</TableCell>
      <TableCell>
        {bad && <Chip size="small" label="malformed" color="error" sx={{ mr: 0.5 }} />}
        {e.st ? <Chip size="small" label={e.st} color={statusColor(e.st)} /> : !bad && "—"}
      </TableCell>
      <TableCell sx={{ maxWidth: 240, wordBreak: "break-word" }}>
        {bad ? (
          <>
            <Typography component="span" variant="body2" sx={{ fontStyle: "italic" }}>
              {e.reason ?? "—"}
            </Typography>
            {e.rawHex && (
              <>
                {" "}
                <Button size="small" sx={{ minWidth: 0, p: 0, textTransform: "none" }} onClick={() => setShowRaw((v) => !v)}>
                  raw
                </Button>
                {showRaw && (
                  <Typography
                    component="div"
                    variant="caption"
                    sx={{ fontFamily: "monospace", wordBreak: "break-all" }}
                  >
                    {e.rawHex}
                  </Typography>
                )}
              </>
            )}
          </>
        ) : (
          (e.body ?? "—")
        )}
      </TableCell>
    </TableRow>
  );
}

function DeviceInner() {
  const id = useDeviceId();
  const router = useRouter();
  const [device, setDevice] = useState<(DeviceDoc & { id: string }) | null>(null);
  const { isSuper, isFamilyAdmin } = useAuth();
  // A super uses the account-level admin routes; a family admin the family ones.
  const fwScope = isSuper ? "admin" : "family";
  const newestBuild = useNewestBuild(isFamilyAdmin, fwScope, id);
  const [fwOpen, setFwOpen] = useState(false);
  const [deviceError, setDeviceError] = useState<string | null>(null);

  const [contacts, setContacts] = useState<SmsContact[] | null>(null);
  const [pending, setPending] = useState(false);
  const [contactsError, setContactsError] = useState<string | null>(null);

  const [log, setLog] = useState<SmsLogEntry[] | null>(null);
  const [logError, setLogError] = useState<string | null>(null);
  const [logLoadingMore, setLogLoadingMore] = useState(false);
  const [logHasMore, setLogHasMore] = useState(true);

  // Device header -- direct Firestore listener, allowed for the owner or an
  // admin (`firestore.rules`' `devices/{d}` read rule).
  useEffect(() => {
    if (!id || id === "_") return;
    const db = getFirestoreDb();
    const unsub = onSnapshot(
      doc(db, "devices", id),
      (snap) => {
        if (!snap.exists()) {
          setDevice(null);
          setDeviceError("No such device.");
          return;
        }
        setDeviceError(null);
        setDevice({ id: snap.id, ...(snap.data() as DeviceDoc) });
      },
      () => setDeviceError("You don't have access to this device.")
    );
    return unsub;
  }, [id]);

  // Both fetchers below set state only after their `await` -- never
  // synchronously on entry -- so they are safe to call unconditionally from
  // the mount effect below (eslint's `react-hooks/set-state-in-effect`
  // flags a setState that would run synchronously as part of the effect's
  // own execution; a `finally`/pre-request "loading" flag would trip it, so
  // that lives only in `loadMore`, a plain click handler, below).
  const fetchContacts = useCallback(async () => {
    if (!id || id === "_") return;
    try {
      const resp = await api.get<SmsContactsResponse>(`/devices/${id}/sms-contacts`);
      setContacts(resp.contacts);
      setPending(resp.pending);
      setContactsError(null);
    } catch (err) {
      setContactsError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load SMS contacts."
      );
    }
  }, [id]);

  const fetchLog = useCallback(
    async (before?: number) => {
      if (!id || id === "_") return;
      const qs = new URLSearchParams({ limit: String(LOG_PAGE_SIZE) });
      if (before !== undefined) qs.set("before", String(before));
      try {
        const resp = await api.get<SmsLogResponse>(`/devices/${id}/sms-log?${qs.toString()}`);
        setLog((prev) => (before === undefined ? resp.entries : [...(prev ?? []), ...resp.entries]));
        setLogHasMore(resp.entries.length >= LOG_PAGE_SIZE);
        setLogError(null);
      } catch (err) {
        setLogError(
          err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load the SMS log."
        );
      }
    },
    [id]
  );

  // docs/V02_DESIGN.md §6: `fetchContacts`/`fetchLog` are a one-shot
  // GET-on-mount for a paginated REST resource, not a Firestore listener
  // this app could otherwise subscribe to. Both only setState after their
  // `await`, never synchronously, but the lint's static analysis flags any
  // function reachable from an effect that *can* eventually setState,
  // regardless of the await boundary -- the same shape
  // `lib/auth-context.tsx`'s `onAuthStateChanged` callback uses (not
  // flagged there only because it is passed to a subscribe-style API
  // rather than called directly). There is no data-fetching library in
  // this app to hand this off to instead (see `web/README.md`'s stack
  // list).
  useEffect(() => {
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void fetchContacts();
    void fetchLog(undefined);
  }, [fetchContacts, fetchLog]);

  const lastLogTs = log && log.length > 0 ? log[log.length - 1].ts : undefined;

  // Click handler only (never called from an effect) -- the "loading"
  // flag needs to be set before the request starts, which is fine here
  // since it is triggered by a user action, not by an effect running.
  async function loadMore() {
    if (lastLogTs === undefined) return;
    setLogLoadingMore(true);
    try {
      await fetchLog(lastLogTs);
    } finally {
      setLogLoadingMore(false);
    }
  }

  const smsLost = device?.status?.smsLost ?? 0;
  // docs/FAMILIES_DESIGN.md §5.4: the read-only note names the owner's
  // alias -- resolved from the directory the same way every other peer name
  // in this app is (`lib/directory.tsx`), not a second Firestore read.
  const ownerAlias = useDirectory().byUid(device?.ownerUid ?? "")?.alias;
  const xportChip = xportChipInfo(device?.status?.xport);

  return (
    <Stack spacing={2}>
      <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
        <IconButton onClick={() => router.back()} aria-label="back" size="small">
          <ArrowBackIcon fontSize="small" />
        </IconButton>
        <Typography variant="h5" sx={{ flexGrow: 1 }}>
          {device?.label ?? id} <Typography component="span" variant="body2" color="text.secondary">({id})</Typography>
        </Typography>
        <DeviceTrustChip tls={device?.status?.tls} caFp={device?.status?.caFp} />
        {device?.status?.car && <Chip size="small" variant="outlined" label={device.status.car} />}
        {xportChip && <Chip size="small" label={xportChip.label} color={xportChip.color} />}
      </Stack>

      {deviceError && <Alert severity="warning">{deviceError}</Alert>}

      {device && (
        <Stack direction="row" spacing={1} sx={{ alignItems: "flex-start" }}>
          <FirmwareChip status={device.status} newest={newestBuild} />
          {isFamilyAdmin && (
            <Button size="small" onClick={() => setFwOpen(true)}>
              Update firmware…
            </Button>
          )}
        </Stack>
      )}
      {fwOpen && <FirmwareUpdateDialog device={device} scope={fwScope} open onClose={() => setFwOpen(false)} />}

      {smsLost > 0 && (
        <Alert severity="warning">
          {smsLost} SMS log {smsLost === 1 ? "entry was" : "entries were"} lost on the pager before
          they could be uploaded.
        </Alert>
      )}

      {id && id !== "_" && <WifiPanel deviceId={id} />}

      {id && id !== "_" && <GnssPanel deviceId={id} />}

      {id && id !== "_" && <BatteryCard deviceId={id} />}

      <Card variant="outlined">
        <CardContent>
          <Typography variant="h6" gutterBottom>
            SMS contacts
          </Typography>
          {device?.status?.tls === "proxy" && (
            <Typography variant="body2" sx={{ mb: 1 }}>
              Device SMS unavailable on this carrier; relay SMS still works
            </Typography>
          )}
          <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
            The pager can text only these numbers, and only they can text it. Every text in either
            direction is recorded below. Managed from People &rarr; @{ownerAlias ?? "..."} &rarr;
            Approved numbers.
          </Typography>

          {pending && <Chip size="small" color="warning" label="waiting for the pager to confirm" sx={{ mb: 2 }} />}
          {contactsError && (
            <Alert severity="error" sx={{ mb: 2 }}>
              {contactsError}
            </Alert>
          )}

          {contacts === null ? (
            <CircularProgress size={24} />
          ) : contacts.length === 0 ? (
            <Typography variant="body2" color="text.secondary">
              No approved numbers yet.
            </Typography>
          ) : (
            <TableContainer>
              <Table size="small">
                <TableHead>
                  <TableRow>
                    <TableCell>Name</TableCell>
                    <TableCell>Phone</TableCell>
                  </TableRow>
                </TableHead>
                <TableBody>
                  {contacts.map((c, i) => (
                    <TableRow key={i}>
                      <TableCell>{c.name}</TableCell>
                      <TableCell>{c.phone}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </TableContainer>
          )}
        </CardContent>
      </Card>

      <Card variant="outlined">
        <CardContent>
          <Stack direction="row" sx={{ alignItems: "center", mb: 1 }}>
            <Typography variant="h6" sx={{ flexGrow: 1 }}>
              SMS log
            </Typography>
            <Button size="small" onClick={() => void fetchLog(undefined)}>
              Refresh
            </Button>
          </Stack>
          {logError && (
            <Alert severity="error" sx={{ mb: 2 }}>
              {logError}
            </Alert>
          )}
          {log !== null && log.length === 0 && !logError && (
            <Typography variant="body2" color="text.secondary">
              No SMS activity yet.
            </Typography>
          )}
          {log !== null && log.length > 0 && (
            <TableContainer sx={{ overflowX: "auto" }}>
              <Table size="small">
                <TableHead>
                  <TableRow>
                    <TableCell>Time</TableCell>
                    <TableCell />
                    <TableCell>Who</TableCell>
                    <TableCell>Status</TableCell>
                    <TableCell>Text</TableCell>
                  </TableRow>
                </TableHead>
                <TableBody>
                  {log.map((e) => (
                    <LogRow key={e.id} e={e} />
                  ))}
                </TableBody>
              </Table>
            </TableContainer>
          )}
          {log !== null && logHasMore && log.length > 0 && (
            <Box sx={{ mt: 2, textAlign: "center" }}>
              <Button
                size="small"
                onClick={() => void loadMore()}
                disabled={logLoadingMore}
              >
                Load more
              </Button>
            </Box>
          )}
          {log === null && <CircularProgress size={24} />}
        </CardContent>
      </Card>
    </Stack>
  );
}

export default function DevicePageClient() {
  return (
    <RequireAuth>
      <AppShell>
        <DeviceInner />
      </AppShell>
    </RequireAuth>
  );
}

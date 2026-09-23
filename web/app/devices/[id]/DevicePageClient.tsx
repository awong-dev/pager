"use client";

/** `/devices/[id]` -- docs/V02_DESIGN.md §6 (device-direct SMS): a contacts
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
 *   PUT  /api/devices/{id}/sms-contacts body {"contacts":[...]} -> same shape; 422 on bad input
 *   GET  /api/devices/{id}/sms-log?limit=100&before=<epoch s> -> {"entries":[...]} newest first
 * The device header (label, status, `smsLost`) comes from a direct
 * Firestore listener on `devices/{id}` instead (allowed for the owner or an
 * admin by `firestore.rules`), matching this app's "reads are Firestore
 * listeners" convention -- only the SMS contacts/log, which need
 * PUT-validation and cursor pagination that a plain `onSnapshot` doesn't
 * give you, go through the API.
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
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import DeleteIcon from "@mui/icons-material/Delete";

import AppShell from "@/components/AppShell";
import DeviceTrustChip from "@/components/DeviceTrustChip";
import RequireAuth from "@/components/RequireAuth";
import WifiPanel from "@/components/WifiPanel";
import { ApiError, api } from "@/lib/api";
import { xportChipInfo } from "@/lib/deviceTrust";
import { getFirestoreDb } from "@/lib/firebase";
import {
  PHONE_E164_EXAMPLE,
  SMS_CONTACT_MAX,
  SMS_NAME_MAX,
  type SmsContact,
  type SmsContactsResponse,
  type SmsLogEntry,
  type SmsLogResponse,
  dirArrow,
  isValidName,
  isValidPhone,
  statusColor,
  validateContacts,
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

function DeviceInner() {
  const id = useDeviceId();
  const router = useRouter();
  const [device, setDevice] = useState<(DeviceDoc & { id: string }) | null>(null);
  const [deviceError, setDeviceError] = useState<string | null>(null);

  const [contacts, setContacts] = useState<SmsContact[] | null>(null);
  const [pending, setPending] = useState(false);
  const [contactsError, setContactsError] = useState<string | null>(null);
  const [validationErrors, setValidationErrors] = useState<string[]>([]);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

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

  function updateContact(index: number, patch: Partial<SmsContact>) {
    setContacts((prev) => {
      if (!prev) return prev;
      const next = [...prev];
      next[index] = { ...next[index], ...patch };
      return next;
    });
    setSaved(false);
  }

  function removeContact(index: number) {
    setContacts((prev) => (prev ? prev.filter((_, i) => i !== index) : prev));
    setSaved(false);
  }

  function addContact() {
    setContacts((prev) => [...(prev ?? []), { name: "", phone: "" }]);
    setSaved(false);
  }

  async function saveContacts() {
    if (!contacts) return;
    const errors = validateContacts(contacts);
    setValidationErrors(errors);
    if (errors.length > 0) return;
    setSaving(true);
    setContactsError(null);
    try {
      const resp = await api.put<SmsContactsResponse>(`/devices/${id}/sms-contacts`, { contacts });
      setContacts(resp.contacts);
      setPending(resp.pending);
      setSaved(true);
    } catch (err) {
      setContactsError(
        err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to save SMS contacts."
      );
    } finally {
      setSaving(false);
    }
  }

  const smsLost = device?.status?.smsLost ?? 0;
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
        {xportChip && <Chip size="small" label={xportChip.label} color={xportChip.color} />}
      </Stack>

      {deviceError && <Alert severity="warning">{deviceError}</Alert>}

      {smsLost > 0 && (
        <Alert severity="warning">
          {smsLost} SMS log {smsLost === 1 ? "entry was" : "entries were"} lost on the pager before
          they could be uploaded.
        </Alert>
      )}

      {id && id !== "_" && <WifiPanel deviceId={id} />}

      <Card variant="outlined">
        <CardContent>
          <Typography variant="h6" gutterBottom>
            SMS contacts
          </Typography>
          <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
            The pager can text only these numbers, and only they can text it. Only you can change
            this list. Every text in either direction is recorded below.
          </Typography>

          {pending && <Chip size="small" color="warning" label="waiting for the pager to confirm" sx={{ mb: 2 }} />}
          {contactsError && (
            <Alert severity="error" sx={{ mb: 2 }}>
              {contactsError}
            </Alert>
          )}
          {validationErrors.length > 0 && (
            <Alert severity="error" sx={{ mb: 2 }}>
              <Stack spacing={0.5}>
                {validationErrors.map((e, i) => (
                  <span key={i}>{e}</span>
                ))}
              </Stack>
            </Alert>
          )}
          {saved && validationErrors.length === 0 && !contactsError && (
            <Alert severity="success" sx={{ mb: 2 }} onClose={() => setSaved(false)}>
              Saved.
            </Alert>
          )}

          {contacts === null ? (
            <CircularProgress size={24} />
          ) : (
            <Stack spacing={1.5}>
              {contacts.map((c, i) => {
                const nameOk = isValidName(c.name);
                const phoneOk = isValidPhone(c.phone);
                return (
                  <Stack key={i} direction="row" spacing={1} sx={{ alignItems: "flex-start", flexWrap: "wrap" }}>
                    <TextField
                      size="small"
                      label="Name"
                      value={c.name}
                      onChange={(e) => updateContact(i, { name: e.target.value })}
                      error={c.name.length > 0 && !nameOk}
                      helperText={`1-${SMS_NAME_MAX} chars`}
                      sx={{ minWidth: 140 }}
                    />
                    <TextField
                      size="small"
                      label="Phone"
                      value={c.phone}
                      onChange={(e) => updateContact(i, { phone: e.target.value })}
                      error={c.phone.length > 0 && !phoneOk}
                      helperText={`e.g. ${PHONE_E164_EXAMPLE}`}
                      sx={{ minWidth: 180 }}
                    />
                    <IconButton
                      aria-label="remove contact"
                      onClick={() => removeContact(i)}
                      sx={{ mt: 0.5 }}
                    >
                      <DeleteIcon fontSize="small" />
                    </IconButton>
                  </Stack>
                );
              })}

              <Stack direction="row" spacing={2} sx={{ mt: 1 }}>
                <Button size="small" onClick={addContact} disabled={contacts.length >= SMS_CONTACT_MAX}>
                  Add contact
                </Button>
                <Button
                  size="small"
                  variant="contained"
                  onClick={() => void saveContacts()}
                  disabled={saving}
                >
                  Save
                </Button>
              </Stack>
              {contacts.length >= SMS_CONTACT_MAX && (
                <Typography variant="caption" color="text.secondary">
                  Maximum {SMS_CONTACT_MAX} contacts.
                </Typography>
              )}
            </Stack>
          )}
        </CardContent>
      </Card>

      <Card variant="outlined">
        <CardContent>
          <Typography variant="h6" gutterBottom>
            SMS log
          </Typography>
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
                    <TableRow
                      key={e.id}
                      sx={{
                        bgcolor:
                          e.st === "blocked"
                            ? "warning.light"
                            : e.st === "failed"
                              ? "error.light"
                              : undefined,
                      }}
                    >
                      <TableCell>{formatLogTs(e.smsTs || e.ts)}</TableCell>
                      <TableCell>{dirArrow(e.dir)}</TableCell>
                      <TableCell>{e.name ?? e.peer}</TableCell>
                      <TableCell>
                        <Chip size="small" label={e.st} color={statusColor(e.st)} />
                      </TableCell>
                      <TableCell sx={{ maxWidth: 240, wordBreak: "break-word" }}>{e.body}</TableCell>
                    </TableRow>
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

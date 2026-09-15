"use client";

/** `/admin/contacts` -- docs/DEVICE_PLAN.md §4.3, docs/DEVICE_TASKS.md W4.4:
 * the pending `contactRequests` queue (docs/DEVICE_PLAN.md §4.2 -- a device
 * asks to add a contact, an admin decides). Approval is admin-only "the
 * device owner is the student, who must not be able to approve their own
 * recipients" (§4.3) -- there is no self-service path here at all.
 *
 * Reads `contactRequests` and `users` straight from Firestore (unfiltered
 * collection listeners, admin-gated by `firestore.rules`' `isAdmin()` --
 * the same pattern `admin/allowlist/page.tsx` already uses for its `allow`
 * collection); every decision is a write to `relay/app/routers/admin.py`'s
 * `POST /admin/contacts/{key}/approve|reject`.
 *
 * **"Link existing user suggested from `phoneIndex` match" (this task's
 * `Do` list), and the gap that shapes how it's implemented:**
 * `docs/SERVER_PLAN.md` §3's rules sketch (and the real `relay/firestore.
 * rules`) makes `phoneIndex` server-only -- "None of the three is
 * client-readable ... default-deny read" -- on purpose (§3: it is inbound
 * SMS routing state, not something a browser should be able to enumerate).
 * So this page cannot read `phoneIndex` itself to suggest a link the way
 * `relay/app/routers/admin.py`'s `_resolve_link_uid` does server-side.
 * What it reads instead, as an admin (who can read every `users/{uid}/
 * backends/{b}` doc, `firestore.rules`): each candidate user's `backends`
 * subcollection, looking for an enabled `sms` backend whose `config.phone`
 * matches and whose `verifiedAt` is set -- the exact condition
 * `app/store/backends.py`'s `set_phone_index` is written under ("Write on
 * verify, never on create"), so the result set this produces is the same
 * one `phoneIndex` holds, just reconstructed from client-readable documents
 * instead of the (deliberately unreadable) index itself. This is a
 * household-scale admin page, so scanning every user's small `backends`
 * subcollection on dialog-open (not a live listener) is cheap.
 *
 * The final decision is always re-validated server-side regardless of what
 * this page suggests -- `approve_contact` 400s if `mode:"link"` cannot
 * actually resolve to a user.
 */

import {
  type Timestamp,
  collection,
  getDocs,
  onSnapshot,
} from "firebase/firestore";
import { useEffect, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import FormControlLabel from "@mui/material/FormControlLabel";
import MenuItem from "@mui/material/MenuItem";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import Stack from "@mui/material/Stack";
import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import AppShell from "@/components/AppShell";
import RequireAuth from "@/components/RequireAuth";
import { ApiError, api } from "@/lib/api";
import { getFirestoreDb } from "@/lib/firebase";
import type { BackendDoc, UserDoc } from "@/lib/types";

// `contactRequests/{deviceId}_{reqId}` -- `relay/app/store/contacts.py`'s
// `ContactRequest` pydantic model. Not part of `lib/types.ts` (outside this
// task's `Files` list, docs/DEVICE_TASKS.md W4.4) -- declared locally, same
// precedent as `admin/devices/page.tsx`'s local `ProvisionState`.
type ContactStatus = "pending" | "approved" | "rejected";

interface ContactRequestDoc {
  deviceId: string;
  reqId: string;
  ownerUid: string;
  name: string;
  phone: string | null;
  alias: string | null;
  status: ContactStatus;
  reason: string | null;
  createdAt: Timestamp | null;
  decidedAt: Timestamp | null;
  decidedBy: string | null;
}

interface ContactRequestRow extends ContactRequestDoc {
  key: string; // Firestore doc id, "{deviceId}_{reqId}"
}

interface UserRow {
  uid: string;
  alias: string;
}

type ApproveMode = "link" | "create";

// Mirrors `relay/app/routers/admin.py`'s `_slugify_name`: ASCII letters and
// digits only, lowercased, truncated to 16 -- the "$ALIAS_RE without the
// leading-character special case" shape that function's own docstring
// describes. Empty for an all-CJK (or otherwise non-ASCII-alnum) name,
// which the caller treats as "the admin must type one" (docs/DEVICE_PLAN.md
// §4.3).
function slugifyName(name: string): string {
  return name
    .toLowerCase()
    .split("")
    .filter((ch) => /[a-z0-9]/.test(ch))
    .join("")
    .slice(0, 16);
}

// Same shape as `relay/app/store/users.py`'s `ALIAS_RE` -- used here only
// for client-side "is Approve enabled yet" validation; the relay is the
// real authority and 400s on anything this misses.
const ALIAS_RE = /^[a-z0-9][a-z0-9_-]{0,15}$/;

function formatAge(ts: Timestamp | null): string {
  if (!ts) return "just now";
  const ms = Date.now() - ts.toDate().getTime();
  if (ms < 60_000) return "just now";
  const mins = Math.floor(ms / 60_000);
  if (mins < 60) return `${mins}m ago`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}h ago`;
  return `${Math.floor(hours / 24)}d ago`;
}

/** See this file's header comment: reconstructs a `phoneIndex` match from
 * client-readable `users/{uid}/backends` subcollections, one-shot (not a
 * listener) since it only runs when the approve dialog opens. */
async function findVerifiedSmsOwner(
  users: UserRow[],
  phone: string
): Promise<UserRow | null> {
  const db = getFirestoreDb();
  const hits = await Promise.all(
    users.map(async (user) => {
      const snap = await getDocs(collection(db, "users", user.uid, "backends"));
      for (const d of snap.docs) {
        const data = d.data() as BackendDoc;
        const config = data.config as { phone?: string };
        if (
          data.kind === "sms" &&
          data.enabled &&
          data.verifiedAt !== null &&
          config.phone === phone
        ) {
          return user;
        }
      }
      return null;
    })
  );
  return hits.find((u): u is UserRow => u !== null) ?? null;
}

function ContactsInner() {
  const [requests, setRequests] = useState<ContactRequestRow[]>([]);
  const [users, setUsers] = useState<UserRow[]>([]);
  const [error, setError] = useState<string | null>(null);

  const [approveTarget, setApproveTarget] = useState<ContactRequestRow | null>(null);
  const [mode, setMode] = useState<ApproveMode>("create");
  const [linkAlias, setLinkAlias] = useState("");
  const [createAlias, setCreateAlias] = useState("");
  const [locate, setLocate] = useState(false);
  const [resolvingLink, setResolvingLink] = useState(false);
  const [submitting, setSubmitting] = useState(false);

  const [rejectTarget, setRejectTarget] = useState<ContactRequestRow | null>(null);
  const [reason, setReason] = useState("not_allowed");

  useEffect(() => {
    const db = getFirestoreDb();
    const unsubRequests = onSnapshot(collection(db, "contactRequests"), (snap) => {
      const rows: ContactRequestRow[] = [];
      snap.forEach((d) => rows.push({ key: d.id, ...(d.data() as ContactRequestDoc) }));
      setRequests(rows);
    });
    const unsubUsers = onSnapshot(collection(db, "users"), (snap) => {
      const rows: UserRow[] = [];
      snap.forEach((d) => rows.push({ uid: d.id, alias: (d.data() as UserDoc).alias }));
      setUsers(rows);
    });
    return () => {
      unsubRequests();
      unsubUsers();
    };
  }, []);

  const pending = requests
    .filter((r) => r.status === "pending")
    .sort((a, b) => (a.createdAt?.toMillis() ?? 0) - (b.createdAt?.toMillis() ?? 0));

  function openApprove(request: ContactRequestRow) {
    setError(null);
    setApproveTarget(request);
    setLocate(false);
    setCreateAlias(slugifyName(request.name));

    const aliasMatch = request.alias ? users.find((u) => u.alias === request.alias) : undefined;
    if (aliasMatch) {
      setMode("link");
      setLinkAlias(aliasMatch.alias);
    } else {
      setMode("create");
      setLinkAlias("");
    }

    if (request.phone) {
      setResolvingLink(true);
      void findVerifiedSmsOwner(users, request.phone).then((match) => {
        setResolvingLink(false);
        if (match) {
          setMode("link");
          setLinkAlias(match.alias);
        }
      });
    }
  }

  function closeApprove() {
    setApproveTarget(null);
    setResolvingLink(false);
  }

  async function submitApprove() {
    if (!approveTarget) return;
    setSubmitting(true);
    setError(null);
    try {
      const alias = mode === "link" ? linkAlias.trim() : createAlias.trim();
      await api.post(`/admin/contacts/${approveTarget.key}/approve`, {
        mode,
        alias: alias || null,
        locate,
      });
      closeApprove();
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to approve contact");
    } finally {
      setSubmitting(false);
    }
  }

  async function submitReject() {
    if (!rejectTarget) return;
    setSubmitting(true);
    setError(null);
    try {
      await api.post(`/admin/contacts/${rejectTarget.key}/reject`, {
        reason: reason.trim() || "not_allowed",
      });
      setRejectTarget(null);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to reject contact");
    } finally {
      setSubmitting(false);
    }
  }

  const approveValid =
    mode === "link" ? linkAlias.trim().length > 0 : ALIAS_RE.test(createAlias.trim());

  return (
    <Stack spacing={2}>
      <Typography variant="h5">Contacts</Typography>
      <Typography variant="body2" color="text.secondary">
        Pending address-book requests from every device. Approving links to an existing user or
        creates a new one; rejecting stores a reason the device shows the student.
      </Typography>
      {error && <Alert severity="error">{error}</Alert>}

      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell>Device</TableCell>
            <TableCell>Name</TableCell>
            <TableCell>Phone / alias</TableCell>
            <TableCell>Age</TableCell>
            <TableCell />
          </TableRow>
        </TableHead>
        <TableBody>
          {pending.length === 0 && (
            <TableRow>
              <TableCell colSpan={5}>
                <Typography variant="body2" color="text.secondary">
                  No pending requests.
                </Typography>
              </TableCell>
            </TableRow>
          )}
          {pending.map((r) => (
            <TableRow key={r.key}>
              <TableCell>{r.deviceId}</TableCell>
              <TableCell>{r.name}</TableCell>
              <TableCell>{r.phone ?? (r.alias ? `@${r.alias}` : "--")}</TableCell>
              <TableCell>{formatAge(r.createdAt)}</TableCell>
              <TableCell>
                <Stack direction="row" spacing={1}>
                  <Button size="small" variant="contained" onClick={() => openApprove(r)}>
                    Approve
                  </Button>
                  <Button
                    size="small"
                    color="warning"
                    onClick={() => {
                      setError(null);
                      setReason("not_allowed");
                      setRejectTarget(r);
                    }}
                  >
                    Reject
                  </Button>
                </Stack>
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>

      <Dialog open={approveTarget !== null} onClose={closeApprove} fullWidth maxWidth="xs">
        <DialogTitle>Approve contact{approveTarget ? `: ${approveTarget.name}` : ""}</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            {resolvingLink && (
              <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                <CircularProgress size={16} />
                <Typography variant="body2" color="text.secondary">
                  Checking for a matching existing user...
                </Typography>
              </Stack>
            )}
            <RadioGroup
              value={mode}
              onChange={(e) => setMode(e.target.value as ApproveMode)}
            >
              <FormControlLabel value="link" control={<Radio />} label="Link to existing user" />
              {mode === "link" && (
                <TextField
                  select
                  label="Existing user"
                  value={linkAlias}
                  onChange={(e) => setLinkAlias(e.target.value)}
                  fullWidth
                  size="small"
                  sx={{ ml: 4, mb: 1, width: "calc(100% - 32px)" }}
                >
                  {users.map((u) => (
                    <MenuItem key={u.uid} value={u.alias}>
                      @{u.alias}
                    </MenuItem>
                  ))}
                </TextField>
              )}
              <FormControlLabel value="create" control={<Radio />} label="Create new user" />
              {mode === "create" && (
                <TextField
                  label="Alias"
                  value={createAlias}
                  onChange={(e) => setCreateAlias(e.target.value.toLowerCase())}
                  helperText={
                    createAlias
                      ? ALIAS_RE.test(createAlias)
                        ? " "
                        : "lowercase letters/digits/-/_, starting with a letter or digit"
                      : "no alias could be guessed from the name -- type one"
                  }
                  error={createAlias.length > 0 && !ALIAS_RE.test(createAlias)}
                  fullWidth
                  size="small"
                  sx={{ ml: 4, mb: 1, width: "calc(100% - 32px)" }}
                />
              )}
            </RadioGroup>
            <FormControlLabel
              control={<Checkbox checked={locate} onChange={(e) => setLocate(e.target.checked)} />}
              label="Also allow location requests"
            />
          </Stack>
        </DialogContent>
        <DialogActions>
          <Button onClick={closeApprove}>Cancel</Button>
          <Button onClick={() => void submitApprove()} disabled={!approveValid || submitting}>
            Approve
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={rejectTarget !== null} onClose={() => setRejectTarget(null)} fullWidth maxWidth="xs">
        <DialogTitle>Reject contact{rejectTarget ? `: ${rejectTarget.name}` : ""}</DialogTitle>
        <DialogContent>
          <TextField
            label="Reason"
            value={reason}
            onChange={(e) => setReason(e.target.value)}
            fullWidth
            sx={{ mt: 1 }}
            helperText="Shown in grey on the device, e.g. &quot;not_allowed&quot; or free text."
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRejectTarget(null)}>Cancel</Button>
          <Button color="warning" onClick={() => void submitReject()} disabled={submitting}>
            Reject
          </Button>
        </DialogActions>
      </Dialog>
    </Stack>
  );
}

export default function ContactsPage() {
  return (
    <RequireAuth requireAdmin>
      <AppShell>
        <ContactsInner />
      </AppShell>
    </RequireAuth>
  );
}

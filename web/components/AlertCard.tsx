"use client";

/** One row in `/family/alerts` -- docs/FAMILIES_DESIGN.md §5.4 Alerts. Three
 * kinds, each with its own icon and action set, all hitting the same three
 * relay endpoints (`POST /api/family/alerts/{id}/{approve|block|dismiss}`,
 * docs/FAMILIES_TASKS.md 4.4):
 *
 * - `sms_unknown`: an SMS from a number with no `allow` edge, held pending a
 *   decision. Approve (name + which member it's for) releases the held text
 *   and creates the contact; Block/Dismiss never release it.
 * - `new_conversation`: a member's out-of-policy DM, created anyway per
 *   their `policy.out: open` but flagged for review. View opens the
 *   family-admin read-only monitor thread (`/chat/view/{convKey}`, task
 *   2.6); Approve adds the `allow` edge so future messages aren't flagged.
 * - `contact_request`: a pager's `contact_req`, the same decision
 *   `/admin/contacts` used to host (docs/DEVICE_PLAN.md §4.3) -- link to an
 *   existing family member or create a new one, moved here per task 4.4.
 *   Its "reject" (`/admin/contacts/{key}/reject`, a `{reason}` body) has no
 *   equivalent on the new generic `block` endpoint (`{}`, no reason) --
 *   Block is used instead, per this task's endpoint list.
 *
 * The old `/admin/contacts` also suggested a "link" target by scanning every
 * candidate's `users/{uid}/backends` for a verified `sms` backend matching
 * the request's phone (see that file's header comment) -- a family admin can
 * no longer read another member's `backends` subcollection under the v2
 * rules (docs/FAMILIES_TASKS.md 1.4: "stays self or super"), so that
 * auto-suggestion is dropped here; the admin picks from the family member
 * list themselves.
 */

import { useState } from "react";
import Link from "next/link";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardActions from "@mui/material/CardActions";
import CardContent from "@mui/material/CardContent";
import Checkbox from "@mui/material/Checkbox";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import FormControlLabel from "@mui/material/FormControlLabel";
import MenuItem from "@mui/material/MenuItem";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import ChatBubbleOutlineIcon from "@mui/icons-material/ChatBubbleOutlined";
import PersonAddIcon from "@mui/icons-material/PersonAdd";
import SmsIcon from "@mui/icons-material/Sms";

import { ApiError, api } from "@/lib/api";
import { useDirectory } from "@/lib/directory";
import { familyQuery } from "@/lib/family-context";
import type { AlertDoc } from "@/lib/types";

export interface AlertRow extends AlertDoc {
  id: string;
}

// Same shape as `app/admin/contacts/page.tsx`'s `ApproveMode` (that page is
// deleted in a later task, docs/FAMILIES_TASKS.md 5.x -- not imported from
// there).
type ApproveMode = "link" | "create";

const ALIAS_RE = /^[a-z0-9][a-z0-9_-]{0,15}$/;

function formatAge(ts: AlertDoc["ts"]): string {
  if (!ts) return "just now";
  const ms = Date.now() - ts.toDate().getTime();
  if (ms < 60_000) return "just now";
  const mins = Math.floor(ms / 60_000);
  if (mins < 60) return `${mins}m ago`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}h ago`;
  return `${Math.floor(hours / 24)}d ago`;
}

function kindIcon(kind: AlertDoc["kind"]) {
  switch (kind) {
    case "sms_unknown":
      return <SmsIcon fontSize="small" />;
    case "new_conversation":
      return <ChatBubbleOutlineIcon fontSize="small" />;
    case "contact_request":
      return <PersonAddIcon fontSize="small" />;
  }
}

function kindLabel(kind: AlertDoc["kind"]): string {
  switch (kind) {
    case "sms_unknown":
      return "Unknown number";
    case "new_conversation":
      return "New conversation";
    case "contact_request":
      return "Contact request";
  }
}

export default function AlertCard({ alert }: { alert: AlertRow }) {
  const { contacts } = useDirectory();
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  // sms_unknown approve dialog.
  const [smsOpen, setSmsOpen] = useState(false);
  const [smsName, setSmsName] = useState("");

  // contact_request approve dialog.
  const [contactOpen, setContactOpen] = useState(false);
  const [mode, setMode] = useState<ApproveMode>("create");
  const [linkUid, setLinkUid] = useState("");
  const [createAlias, setCreateAlias] = useState("");
  const [locate, setLocate] = useState(false);

  const handled = alert.status !== "open";

  async function post(action: "approve" | "block" | "dismiss", body: unknown) {
    setSubmitting(true);
    setError(null);
    try {
      await api.post(`/family/alerts/${alert.id}/${action}${familyQuery()}`, body);
      setSmsOpen(false);
      setContactOpen(false);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : `Failed to ${action}`);
    } finally {
      setSubmitting(false);
    }
  }

  function openSmsApprove() {
    setError(null);
    setSmsName("");
    setSmsOpen(true);
  }

  function openContactApprove() {
    setError(null);
    setMode(alert.peerAlias ? "link" : "create");
    setLinkUid(alert.peerAlias ? (contacts.find((c) => c.alias === alert.peerAlias)?.uid ?? "") : "");
    setCreateAlias(alert.peerAlias ?? "");
    setLocate(false);
    setContactOpen(true);
  }

  const contactAlias = mode === "link" ? (contacts.find((c) => c.uid === linkUid)?.alias ?? "") : createAlias.trim();
  const contactValid = mode === "link" ? linkUid.length > 0 : ALIAS_RE.test(createAlias.trim());

  return (
    <Card variant="outlined">
      <CardContent>
        <Stack direction="row" spacing={1} sx={{ alignItems: "center", mb: 1 }}>
          <Chip size="small" icon={kindIcon(alert.kind)} label={kindLabel(alert.kind)} />
          <Typography variant="caption" color="text.secondary">
            {formatAge(alert.ts)}
          </Typography>
          {handled && (
            <Chip
              size="small"
              variant="outlined"
              color={alert.status === "dismissed" ? "default" : "success"}
              label={alert.status}
            />
          )}
        </Stack>

        {alert.kind === "sms_unknown" && (
          <>
            <Typography variant="body1">
              {alert.peerPhone ?? "Unknown number"} → @{alert.subjectAlias}
            </Typography>
            {alert.preview && (
              <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                &quot;{alert.preview}&quot;
              </Typography>
            )}
          </>
        )}

        {alert.kind === "new_conversation" && (
          <>
            <Typography variant="body1">
              @{alert.subjectAlias} started a chat with{" "}
              {alert.peerAlias ? `@${alert.peerAlias}` : (alert.peerPhone ?? "someone")}
            </Typography>
            {alert.preview && (
              <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                {alert.preview}
              </Typography>
            )}
          </>
        )}

        {alert.kind === "contact_request" && (
          <>
            <Typography variant="body1">New contact for @{alert.subjectAlias}</Typography>
            {alert.preview && (
              <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                {alert.preview}
              </Typography>
            )}
          </>
        )}

        {error && (
          <Alert severity="error" sx={{ mt: 1 }}>
            {error}
          </Alert>
        )}
      </CardContent>

      {!handled && (
        <CardActions>
          {alert.kind === "sms_unknown" && (
            <Button size="small" variant="contained" onClick={openSmsApprove} disabled={submitting}>
              Approve
            </Button>
          )}
          {alert.kind === "new_conversation" && (
            <>
              {alert.convKey && (
                <Button
                  size="small"
                  component={Link}
                  href={`/chat/view/${encodeURIComponent(alert.convKey)}`}
                >
                  View
                </Button>
              )}
              <Button
                size="small"
                variant="contained"
                onClick={() => void post("approve", {})}
                disabled={submitting}
              >
                Approve
              </Button>
            </>
          )}
          {alert.kind === "contact_request" && (
            <Button size="small" variant="contained" onClick={openContactApprove} disabled={submitting}>
              Approve
            </Button>
          )}
          <Button size="small" color="warning" onClick={() => void post("block", {})} disabled={submitting}>
            Block
          </Button>
          <Button size="small" onClick={() => void post("dismiss", {})} disabled={submitting}>
            Dismiss
          </Button>
        </CardActions>
      )}

      <Dialog open={smsOpen} onClose={() => setSmsOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Approve {alert.peerPhone ?? "number"}</DialogTitle>
        <DialogContent>
          <TextField
            autoFocus
            label="Name"
            value={smsName}
            onChange={(e) => setSmsName(e.target.value)}
            fullWidth
            sx={{ mt: 1 }}
            helperText={`Approved for @${alert.subjectAlias}; releases the held text.`}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setSmsOpen(false)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={submitting || smsName.trim().length === 0}
            onClick={() => void post("approve", { name: smsName.trim(), forAlias: alert.subjectAlias })}
          >
            Approve
          </Button>
        </DialogActions>
      </Dialog>

      <Dialog open={contactOpen} onClose={() => setContactOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Approve contact for @{alert.subjectAlias}</DialogTitle>
        <DialogContent>
          <Stack spacing={2} sx={{ mt: 1 }}>
            <RadioGroup value={mode} onChange={(e) => setMode(e.target.value as ApproveMode)}>
              <FormControlLabel value="link" control={<Radio />} label="Link to existing family member" />
              {mode === "link" && (
                <TextField
                  select
                  label="Existing member"
                  value={linkUid}
                  onChange={(e) => setLinkUid(e.target.value)}
                  fullWidth
                  size="small"
                  sx={{ ml: 4, mb: 1, width: "calc(100% - 32px)" }}
                >
                  {contacts.map((c) => (
                    <MenuItem key={c.uid} value={c.uid}>
                      @{c.alias}
                    </MenuItem>
                  ))}
                </TextField>
              )}
              <FormControlLabel value="create" control={<Radio />} label="Create new person" />
              {mode === "create" && (
                <TextField
                  label="Alias"
                  value={createAlias}
                  onChange={(e) => setCreateAlias(e.target.value.toLowerCase())}
                  helperText={
                    createAlias.length === 0 || ALIAS_RE.test(createAlias.trim())
                      ? " "
                      : "lowercase letters/digits/-/_, starting with a letter or digit"
                  }
                  error={createAlias.length > 0 && !ALIAS_RE.test(createAlias.trim())}
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
          <Button onClick={() => setContactOpen(false)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={!contactValid || submitting}
            onClick={() => void post("approve", { mode, alias: contactAlias || null, locate })}
          >
            Approve
          </Button>
        </DialogActions>
      </Dialog>
    </Card>
  );
}

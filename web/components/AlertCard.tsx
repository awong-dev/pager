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
 * - `contact_request`: a pager's `contact_req`. One-click Approve: an SMS
 *   request creates an SMS contact for the owner, a link request adds the
 *   owner->peer edge; approval never creates a person
 *   (docs/CONTACT_REQ_DESIGN.md decision 2). Block is offered for SMS only.
 */

import { useState } from "react";
import Link from "next/link";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Card from "@mui/material/Card";
import CardActions from "@mui/material/CardActions";
import CardContent from "@mui/material/CardContent";
import Chip from "@mui/material/Chip";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";
import ChatBubbleOutlineIcon from "@mui/icons-material/ChatBubbleOutlined";
import PersonAddIcon from "@mui/icons-material/PersonAdd";
import SmsIcon from "@mui/icons-material/Sms";

import { formatPhoneDigits } from "@/components/NewChatDialog";
import { ApiError, api } from "@/lib/api";
import { familyQuery } from "@/lib/family-context";
import type { AlertDoc } from "@/lib/types";

export interface AlertRow extends AlertDoc {
  id: string;
}

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
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  // sms_unknown approve dialog.
  const [smsOpen, setSmsOpen] = useState(false);
  const [smsName, setSmsName] = useState("");

  const handled = alert.status !== "open";

  async function post(action: "approve" | "block" | "dismiss", body: unknown) {
    setSubmitting(true);
    setError(null);
    try {
      await api.post(`/family/alerts/${alert.id}/${action}${familyQuery()}`, body);
      setSmsOpen(false);
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

  const isSms = alert.peerPhone != null;

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
            {isSms ? (
              <>
                <Typography variant="body1">SMS contact for @{alert.subjectAlias}</Typography>
                <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                  {alert.preview} · {formatPhoneDigits((alert.peerPhone ?? "").replace(/^\+/, ""))}
                </Typography>
                {alert.peerAlias && (
                  <Typography variant="body2" color="text.secondary">
                    already known as {alert.peerName ?? alert.peerAlias} (@{alert.peerAlias})
                  </Typography>
                )}
                <Typography variant="caption" color="text.secondary" sx={{ display: "block", mt: 0.5 }}>
                  Approve: @{alert.subjectAlias}&apos;s pager can text this number.
                </Typography>
              </>
            ) : (
              <>
                <Typography variant="body1">
                  Link @{alert.subjectAlias} to @{alert.peerAlias} ({alert.peerName ?? "unknown"})
                </Typography>
                <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                  asked as {alert.preview}
                </Typography>
                <Typography variant="caption" color="text.secondary" sx={{ display: "block", mt: 0.5 }}>
                  Approve: @{alert.subjectAlias} can message @{alert.peerAlias}.
                </Typography>
              </>
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
            <Button size="small" variant="contained" onClick={() => void post("approve", {})} disabled={submitting}>
              Approve
            </Button>
          )}
          {(alert.kind !== "contact_request" || isSms) && (
            <Button size="small" color="warning" onClick={() => void post("block", {})} disabled={submitting}>
              Block
            </Button>
          )}
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

    </Card>
  );
}

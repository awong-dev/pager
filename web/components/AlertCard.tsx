"use client";

/** One row in `/family/alerts` -- docs/FAMILIES_DESIGN.md §5.4 Alerts. Three
 * kinds, each with its own icon and action set, all hitting the same three
 * relay endpoints (`POST /api/family/alerts/{id}/{approve|block|dismiss}`,
 * docs/FAMILIES_TASKS.md 4.4):
 *
 * - `sms_unknown`: a text the pager received from a number not on its SMS
 *   list (blocked on the device). Approve (name) creates the contact and adds
 *   it to that member's pager SMS list; Block adds the number to the
 *   family's blocked list.
 * - `new_conversation`: a member's out-of-policy DM, created anyway per
 *   their `policy.out: open` but flagged for review. View opens the
 *   family-admin read-only monitor thread (`/chat/view/{convKey}`, task
 *   2.6); Approve adds the `allow` edge so future messages aren't flagged.
 * - `chat_unknown`: a Google Chat conversation the bridge phone saw
 *   (docs/BRIDGE_PHONE_DESIGN.md decision 8). Subscribe opens
 *   `ChatSubscribeDialog` (its endpoint decides the alert), Ignore marks the
 *   conversation ignored, Dismiss drops the held texts; no Block.
 * - `contact_request`: a pager's `contact_req`. One-click Approve: an SMS
 *   request creates an SMS contact for the owner, a link request adds the
 *   owner->peer edge; approval never creates a person
 *   (docs/CONTACT_REQ_DESIGN.md decision 2). Block is offered for SMS only.
 */

import { useState } from "react";
import Collapse from "@mui/material/Collapse";
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
import ForumIcon from "@mui/icons-material/Forum";
import PersonAddIcon from "@mui/icons-material/PersonAdd";
import SmsIcon from "@mui/icons-material/Sms";

import ChatSubscribeDialog from "@/components/ChatSubscribeDialog";
import { formatPhoneDigits } from "@/components/NewChatDialog";
import { ApiError, api } from "@/lib/api";
import { ignoreChat } from "@/lib/bridges";
import { familyQuery } from "@/lib/family-context";
import type { AlertDoc } from "@/lib/types";

/** One row of `GET /api/family/alerts/{id}/held` (docs/RELAY_SMS_DESIGN.md decision 5). */
interface HeldText {
  id: string;
  body: string;
  // Assumed epoch seconds, epoch ms or an ISO string; `formatReceived` takes any.
  receivedAt: number | string | null;
  status: "held" | "delivered" | "too_long" | "blocked" | "dismissed";
}

interface ApproveResult {
  delivered?: number;
  undelivered?: number;
}

function formatReceived(v: HeldText["receivedAt"]): string {
  if (v === null || v === undefined) return "";
  const ms = typeof v === "number" ? (v < 1e12 ? v * 1000 : v) : Date.parse(v);
  return Number.isNaN(ms) ? "" : new Date(ms).toLocaleString();
}

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
    case "chat_unknown":
      return <ForumIcon fontSize="small" />;
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
    case "chat_unknown":
      return "Google Chat";
  }
}

export default function AlertCard({ alert }: { alert: AlertRow }) {
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  // sms_unknown approve dialog.
  const [smsOpen, setSmsOpen] = useState(false);
  const [smsName, setSmsName] = useState("");

  // 409 name collision on a contact_request approve: ask for another name.
  const [nameOpen, setNameOpen] = useState(false);
  const [nameValue, setNameValue] = useState("");
  const [nameConflict, setNameConflict] = useState<string | null>(null);

  // Held-text expander and approve outcome (relay-SMS `sms_unknown` only).
  const [heldOpen, setHeldOpen] = useState(false);
  const [heldList, setHeldList] = useState<HeldText[] | null>(null);
  const [heldError, setHeldError] = useState<string | null>(null);
  const [result, setResult] = useState<ApproveResult | null>(null);

  const heldCount = alert.kind === "sms_unknown" ? (alert.heldCount ?? 0) : 0;
  const hasHeld = heldCount > 0;

  async function toggleHeld() {
    const next = !heldOpen;
    setHeldOpen(next);
    if (!next) return;
    setHeldError(null);
    try {
      const resp = await api.get<{ held: HeldText[] }>(`/family/alerts/${alert.id}/held${familyQuery()}`);
      setHeldList(resp.held);
    } catch (err) {
      setHeldError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to load messages");
    }
  }

  const handled = alert.status !== "open";

  // chat_unknown (docs/BRIDGE_PHONE_DESIGN.md decision 8): Subscribe opens the
  // dialog, whose endpoint decides the alert; Ignore marks the conversation.
  const [subscribeOpen, setSubscribeOpen] = useState(false);
  const [chatResult, setChatResult] = useState<string | null>(null);
  const chatPeople = alert.people ?? [];
  const chatWaiting = alert.heldCount ?? 0;
  const chatReady = Boolean(alert.bridgeId && alert.convRef);

  async function ignoreChatAlert() {
    if (!alert.bridgeId || !alert.convRef) return;
    setSubmitting(true);
    setError(null);
    try {
      await ignoreChat(alert.bridgeId, alert.convRef);
    } catch (err) {
      setError(err instanceof ApiError ? String(err.detail ?? err.message) : "Failed to ignore");
    } finally {
      setSubmitting(false);
    }
  }

  async function post(action: "approve" | "block" | "dismiss", body: unknown) {
    setSubmitting(true);
    setError(null);
    try {
      const resp = await api.post<ApproveResult | null>(`/family/alerts/${alert.id}/${action}${familyQuery()}`, body);
      if (action === "approve" && alert.kind === "sms_unknown" && hasHeld) setResult(resp ?? {});
      setSmsOpen(false);
      setNameOpen(false);
      setNameConflict(null);
    } catch (err) {
      const detail = err instanceof ApiError ? String(err.detail ?? err.message) : `Failed to ${action}`;
      if (err instanceof ApiError && err.status === 409 && action === "approve") {
        if (alert.kind === "contact_request") {
          setNameConflict(detail);
          setNameValue((body as { name?: string }).name ?? alert.preview);
          setNameOpen(true);
        } else if (alert.kind === "sms_unknown") {
          setNameConflict(detail);
        } else {
          setError(detail);
        }
      } else {
        setError(detail);
      }
    } finally {
      setSubmitting(false);
    }
  }

  function openSmsApprove() {
    setError(null);
    setSmsName("");
    setNameConflict(null);
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
              {hasHeld
                ? `to @${alert.subjectAlias} · ${formatPhoneDigits((alert.peerPhone ?? "").replace(/^\+/, ""))}`
                : `${alert.peerPhone ?? "Unknown number"} → @${alert.subjectAlias}`}
            </Typography>
            {alert.preview && (
              <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                &quot;{alert.preview}&quot;
              </Typography>
            )}
            {hasHeld && (
              <>
                <Button size="small" onClick={() => void toggleHeld()} sx={{ mt: 0.5, px: 0 }}>
                  {heldCount} {heldCount === 1 ? "message" : "messages"} waiting {heldOpen ? "▴" : "▾"}
                </Button>
                <Collapse in={heldOpen} unmountOnExit>
                  {heldError && <Alert severity="error">{heldError}</Alert>}
                  {heldList === null && !heldError && (
                    <Typography variant="caption" color="text.secondary">
                      Loading…
                    </Typography>
                  )}
                  <Stack spacing={1} sx={{ mt: 0.5 }}>
                    {heldList?.map((h) => (
                      <div key={h.id}>
                        <Stack direction="row" spacing={1} sx={{ alignItems: "center" }}>
                          <Typography variant="caption" color="text.secondary">
                            {formatReceived(h.receivedAt)}
                          </Typography>
                          {h.status !== "held" && (
                            <Chip size="small" variant="outlined" label={h.status.replace("_", " ")} />
                          )}
                        </Stack>
                        <Typography variant="body2" sx={{ whiteSpace: "pre-wrap" }}>
                          {h.body}
                        </Typography>
                      </div>
                    ))}
                  </Stack>
                </Collapse>
              </>
            )}
            {hasHeld && !handled && (
              <Typography variant="caption" color="text.secondary" sx={{ display: "block", mt: 0.5 }}>
                Block: no more texts from this number reach @{alert.subjectAlias}; the waiting messages are
                discarded. Dismiss: the waiting messages are discarded.
              </Typography>
            )}
            {result && (
              <Alert severity={result.undelivered ? "warning" : "success"} sx={{ mt: 1 }}>
                Delivered {result.delivered ?? 0}
                {result.undelivered ? `; ${result.undelivered} could not be delivered` : ""}
              </Alert>
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

        {alert.kind === "chat_unknown" && (
          <>
            <Typography variant="body1">
              {alert.convTitle || "Untitled conversation"} → @{alert.subjectAlias}
            </Typography>
            <Typography variant="body2" color="text.secondary">
              {alert.isGroup ? `Group (${chatPeople.length} ${chatPeople.length === 1 ? "person" : "people"} seen)` : "Direct message"}
              {alert.source === "gvoice" ? " · Google Voice" : ""}
            </Typography>
            {chatPeople.length > 0 && (
              <Stack direction="row" spacing={0.5} useFlexGap sx={{ mt: 0.5, flexWrap: "wrap" }}>
                {chatPeople.map((p) => (
                  <Chip key={p} size="small" variant="outlined" label={p} />
                ))}
              </Stack>
            )}
            {alert.preview && (
              <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
                &quot;{alert.preview}&quot;
              </Typography>
            )}
            {chatWaiting > 0 && (
              <Typography variant="caption" color="text.secondary" sx={{ display: "block", mt: 0.5 }}>
                {chatWaiting} {chatWaiting === 1 ? "message" : "messages"} waiting
              </Typography>
            )}
            {chatResult && (
              <Alert severity="success" sx={{ mt: 1 }}>
                {chatResult}
              </Alert>
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
          {alert.kind === "chat_unknown" && (
            <>
              <Button size="small" variant="contained" disabled={submitting || !chatReady} onClick={() => setSubscribeOpen(true)}>
                Subscribe
              </Button>
              <Button size="small" disabled={submitting || !chatReady} onClick={() => void ignoreChatAlert()}>
                Ignore
              </Button>
            </>
          )}
          {alert.kind !== "chat_unknown" && (alert.kind !== "contact_request" || isSms) && (
            <Button size="small" color="warning" onClick={() => void post("block", {})} disabled={submitting}>
              Block
            </Button>
          )}
          <Button size="small" onClick={() => void post("dismiss", {})} disabled={submitting}>
            Dismiss
          </Button>
        </CardActions>
      )}

      {alert.kind === "chat_unknown" && alert.bridgeId && alert.convRef && (
        <ChatSubscribeDialog
          ownerUid={alert.subjectUid}
          target={
            subscribeOpen
              ? {
                  bridgeId: alert.bridgeId,
                  ref: alert.convRef,
                  title: alert.convTitle ?? "",
                  isGroup: alert.isGroup ?? false,
                  people: chatPeople,
                }
              : null
          }
          onClose={() => setSubscribeOpen(false)}
          onSubscribed={(res, name) => {
            setSubscribeOpen(false);
            setChatResult(`${name} is on the pager; ${res.delivered} waiting messages delivered`);
          }}
        />
      )}

      <Dialog open={smsOpen} onClose={() => setSmsOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Approve {alert.peerPhone ?? "number"}</DialogTitle>
        <DialogContent>
          {nameConflict && smsOpen && (
            <Alert severity="warning" sx={{ mb: 1 }}>
              {nameConflict}
            </Alert>
          )}
          <TextField
            autoFocus
            label="Name"
            value={smsName}
            onChange={(e) => setSmsName(e.target.value)}
            fullWidth
            sx={{ mt: 1 }}
            helperText={
              hasHeld
                ? `@${alert.subjectAlias} can text this number from their pager, and the ${heldCount} waiting ${heldCount === 1 ? "message is" : "messages are"} delivered now.`
                : `Adds it to @${alert.subjectAlias}'s pager SMS list.`
            }
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

      <Dialog open={nameOpen} onClose={() => setNameOpen(false)} fullWidth maxWidth="xs">
        <DialogTitle>Choose another name</DialogTitle>
        <DialogContent>
          {nameConflict && (
            <Alert severity="warning" sx={{ mb: 1 }}>
              {nameConflict}
            </Alert>
          )}
          <TextField
            autoFocus
            label="Name"
            value={nameValue}
            onChange={(e) => setNameValue(e.target.value)}
            fullWidth
            sx={{ mt: 1 }}
            helperText="Must be unique in the family; the pager shows the first 16 characters."
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setNameOpen(false)}>Cancel</Button>
          <Button
            variant="contained"
            disabled={submitting || nameValue.trim().length === 0}
            onClick={() => void post("approve", { name: nameValue.trim() })}
          >
            Approve
          </Button>
        </DialogActions>
      </Dialog>
    </Card>
  );
}

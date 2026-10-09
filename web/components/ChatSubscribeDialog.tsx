"use client";

/** Subscribe dialog for a seen Google Chat / Voice conversation --
 * docs/BRIDGE_PHONE_DESIGN.md decision 7 and O5. Used by `/family/chat` and by
 * the `chat_unknown` alert card (a successful subscribe also decides the
 * alert, server-side). The member's outbound policy is read live; when its
 * people rule is `none` the "Kid can reply" switch is forced off and disabled
 * (O5), and the request sends `canReply: false`.
 */

import { doc, onSnapshot } from "firebase/firestore";
import { useEffect, useMemo, useState } from "react";
import Alert from "@mui/material/Alert";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import FormControlLabel from "@mui/material/FormControlLabel";
import Stack from "@mui/material/Stack";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import ChatRosterEditor, { rosterErrors } from "@/components/ChatRosterEditor";
import { ApiError } from "@/lib/api";
import { NICK_MAX_CODEPOINTS, nickError, useBook } from "@/lib/book";
import { defaultRoster, outboundBlocksPeople, subscribeChat } from "@/lib/bridges";
import { getFirestoreDb } from "@/lib/firebase";
import type { RosterEntry, SubscribeResult } from "@/lib/types";

export interface SubscribeTarget {
  bridgeId: string;
  ref: string;
  title: string;
  isGroup: boolean;
  /** Speakers seen so far (the roster rows). */
  people: string[];
}

function Body({
  ownerUid,
  target,
  onClose,
  onSubscribed,
}: {
  ownerUid: string;
  target: SubscribeTarget;
  onClose: () => void;
  onSubscribed: (result: SubscribeResult, pagerName: string) => void;
}) {
  const [name, setName] = useState([...target.title].slice(0, NICK_MAX_CODEPOINTS).join(""));
  const [canReply, setCanReply] = useState(true);
  const [roster, setRoster] = useState<RosterEntry[]>(() => defaultRoster(target.people));
  const [busy, setBusy] = useState(false);
  const [failure, setFailure] = useState<string | null>(null);
  const [policyOut, setPolicyOut] = useState<string | null>(null);
  const [alias, setAlias] = useState("");

  useEffect(() => {
    return onSnapshot(doc(getFirestoreDb(), "users", ownerUid), (snap) => {
      const d = snap.data();
      setPolicyOut((d?.policy?.out as string | undefined) ?? null);
      setAlias((d?.alias as string | undefined) ?? "");
    });
  }, [ownerUid]);

  const { data: book } = useBook(ownerUid);
  const pastCap = book !== null && book.entries.length >= book.pagerCap;

  const replyBlocked = policyOut !== null && outboundBlocksPeople(policyOut);
  const nameInvalid = name.trim() === "" ? "Required" : nickError(name);
  const rosterBad = target.isGroup && rosterErrors(roster).some((e) => e !== null);

  async function submit() {
    setBusy(true);
    setFailure(null);
    try {
      const res = await subscribeChat(target.bridgeId, target.ref, {
        pagerName: name.trim(),
        canReply: replyBlocked ? false : canReply,
        roster: target.isGroup ? roster : [],
      });
      onSubscribed(res ?? {}, name.trim());
    } catch (e) {
      setFailure(e instanceof ApiError ? String(e.detail ?? e.message) : "Could not subscribe");
    } finally {
      setBusy(false);
    }
  }

  return (
    <>
      <DialogTitle>Subscribe to {target.title || "this conversation"}</DialogTitle>
      <DialogContent>
        <Stack spacing={2} sx={{ mt: 1 }}>
          {failure && <Alert severity="error">{failure}</Alert>}
          <TextField
            autoFocus
            fullWidth
            label="Name on pager"
            value={name}
            onChange={(e) => setName(e.target.value)}
            error={nameInvalid !== null}
            helperText={nameInvalid ?? `${[...name].length}/${NICK_MAX_CODEPOINTS}`}
          />
          {pastCap && (
            <Alert severity="info">
              The pager holds {book?.pagerCap} entries. This one will be reachable only from the web.
            </Alert>
          )}
          <div>
            <FormControlLabel
              control={
                <Switch
                  checked={replyBlocked ? false : canReply}
                  disabled={replyBlocked}
                  onChange={(e) => setCanReply(e.target.checked)}
                />
              }
              label="Kid can reply"
            />
            {replyBlocked && (
              <Typography variant="caption" color="text.secondary" sx={{ display: "block" }}>
                @{alias}&apos;s policy does not allow outbound messages
              </Typography>
            )}
          </div>
          {target.isGroup && <ChatRosterEditor roster={roster} onChange={setRoster} />}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>Cancel</Button>
        <Button variant="contained" disabled={busy || nameInvalid !== null || rosterBad} onClick={() => void submit()}>
          Subscribe
        </Button>
      </DialogActions>
    </>
  );
}

export default function ChatSubscribeDialog({
  ownerUid,
  target,
  onClose,
  onSubscribed,
}: {
  ownerUid: string;
  /** `null` closes the dialog. */
  target: SubscribeTarget | null;
  onClose: () => void;
  onSubscribed: (result: SubscribeResult, pagerName: string) => void;
}) {
  // Keyed per conversation so the form state initializes fresh from `target`.
  const key = useMemo(() => (target ? `${target.bridgeId}/${target.ref}` : "none"), [target]);
  return (
    <Dialog open={target !== null} onClose={onClose} fullWidth maxWidth="sm">
      {target && <Body key={key} ownerUid={ownerUid} target={target} onClose={onClose} onSubscribed={onSubscribed} />}
    </Dialog>
  );
}

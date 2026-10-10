"use client";

/** `/chat`'s "New chat" dialog -- docs/ADDRESS_BOOK_DESIGN.md decision 12:
 * a search-as-you-type select over the signed-in user's own address book
 * (`GET /api/book`), grouped Family / People / Numbers / Groups. Family
 * admins may also type an unknown @alias (freeSolo). Sendable SMS contacts
 * are listed only when the signed-in user has a relay SMS number
 * (docs/RELAY_SMS_DESIGN.md decision 8). Selecting or submitting an option only navigates
 * to `/chat/{alias}` -- the conversation
 * itself is created by that thread's first send (docs/FAMILIES_TASKS.md
 * 3.5), never by this dialog.
 */

import { doc, onSnapshot } from "firebase/firestore";
import { useEffect, useMemo, useState } from "react";
import Link from "next/link";
import { useRouter } from "next/navigation";
import Alert from "@mui/material/Alert";
import Autocomplete, { createFilterOptions } from "@mui/material/Autocomplete";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import TextField from "@mui/material/TextField";
import PhoneIcon from "@mui/icons-material/Phone";

import { useAuth } from "@/lib/auth-context";
import { getFirestoreDb } from "@/lib/firebase";
import { BOOK_GROUP_ORDER, type BookGroup, bookGroup, useBook } from "@/lib/book";
import { useFullScreenDialog } from "@/lib/useFullScreenDialog";

interface ChatOption {
  /** Route alias -- what `/chat/{alias}` gets pushed to (E.164 digits for
   * a number, the person/group alias otherwise). */
  alias: string;
  label: string;
  group: BookGroup;
  displayName: string;
  phone: string | null;
  /** Admins also see unsendable entries, greyed out. */
  disabled: boolean;
  external: boolean;
}

/** Parses a free-typed value as a phone number, the loose way §5.2 asks
 * for: `+`, digits, spaces, dashes and parens only; a bare 10-digit number
 * is assumed US/Canada (+1), matching the relay's own SMS-backend
 * normalisation (docs/FAMILIES_DESIGN.md §1 decision 6). Returns the E.164
 * digits with no leading `+`, or `null` if `value` isn't phone-shaped
 * (e.g. it's an alias). */
export function parsePhoneDigits(value: string): string | null {
  const trimmed = value.trim();
  if (!trimmed || !/^[+\d][\d\s().-]*$/.test(trimmed)) return null;
  const hasPlus = trimmed.startsWith("+");
  const digits = trimmed.replace(/\D/g, "");
  if (hasPlus) return digits.length >= 7 ? digits : null;
  if (digits.length === 10) return `1${digits}`;
  if (digits.length === 11 && digits.startsWith("1")) return digits;
  return null;
}

/** "+1 555 123 4567" for a US/Canada number, "+<digits>" otherwise -- the
 * only shape `parsePhoneDigits` assumes a country code for is the first. */
export function formatPhoneDigits(digits: string): string {
  if (digits.length === 11 && digits.startsWith("1")) {
    const rest = digits.slice(1);
    return `+1 ${rest.slice(0, 3)} ${rest.slice(3, 6)} ${rest.slice(6)}`;
  }
  return `+${digits}`;
}

const filter = createFilterOptions<ChatOption>({
  ignoreCase: true,
  ignoreAccents: true,
  stringify: (o) => `${o.label} ${o.displayName} ${o.alias} ${o.phone ?? ""}`,
});

interface NewChatDialogProps {
  open: boolean;
  onClose: () => void;
}

export default function NewChatDialog({ open, onClose }: NewChatDialogProps) {
  const fullScreen = useFullScreenDialog();
  const router = useRouter();
  const { isFamilyAdmin, me } = useAuth();
  // Live, so an admin setting the number takes effect without a reload.
  const [hasSmsNumber, setHasSmsNumber] = useState(false);
  const meUid = me?.uid;
  useEffect(() => {
    if (!meUid) return;
    return onSnapshot(doc(getFirestoreDb(), "users", meUid), (snap) => {
      setHasSmsNumber(Boolean(snap.data()?.smsNumber));
    });
  }, [meUid]);
  const { data } = useBook();
  const [inputValue, setInputValue] = useState("");
  const [value, setValue] = useState<ChatOption | string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const entries = useMemo(() => data?.entries ?? [], [data]);
  const options = useMemo<ChatOption[]>(() => {
    const rows: ChatOption[] = [];
    for (const e of entries) {
      // SMS contacts are chat targets only through the user's relay number.
      // A bridged Google Chat contact (`chat` set) needs no relay number; it
      // is listed with the SMS group when sendable (docs/BRIDGE_PHONE_DESIGN.md decision 7).
      if (e.kind === "external" && !e.chat && (!hasSmsNumber || !e.sendable)) continue;
      if (!e.sendable && !isFamilyAdmin) continue;
      rows.push({
        alias: e.alias,
        label: e.kind === "external" && e.phone ? `${e.label} · ${formatPhoneDigits(e.phone.replace(/^\+/, ""))}` : e.label,
        group: bookGroup(e),
        displayName: e.displayName,
        phone: e.phone ?? null,
        disabled: !e.sendable,
        external: e.kind === "external",
      });
    }
    rows.sort(
      (a, b) =>
        BOOK_GROUP_ORDER.indexOf(a.group) - BOOK_GROUP_ORDER.indexOf(b.group) || a.label.localeCompare(b.label)
    );
    return rows;
  }, [entries, isFamilyAdmin, hasSmsNumber]);

  const bookEmpty = data !== null && !entries.some((e) => e.sendable);

  function reset() {
    setInputValue("");
    setValue(null);
    setNotice(null);
  }

  function handleClose() {
    reset();
    onClose();
  }

  function go(alias: string) {
    reset();
    onClose();
    router.push(`/chat/${encodeURIComponent(alias)}`);
  }

  // Admin free text: an alias typed with or without its leading `@`
  // navigates the same way; a phone-shaped value by its E.164 digits.
  function handleSubmit(text: string) {
    const trimmed = text.trim();
    if (!trimmed) return;
    if (parsePhoneDigits(trimmed) !== null) {
      setNotice("Numbers are texted from the pager. Add one under Family → Contacts.");
      return;
    }
    go(trimmed.replace(/^@/, ""));
  }

  function open_() {
    if (value && typeof value !== "string") go(value.alias);
    else handleSubmit(typeof value === "string" ? value : inputValue);
  }

  const canOpen = value !== null || (isFamilyAdmin && inputValue.trim() !== "");

  return (
    <Dialog fullScreen={fullScreen} open={open} onClose={handleClose} fullWidth maxWidth="xs">
      <DialogTitle>New chat</DialogTitle>
      <DialogContent>
        {bookEmpty && (
          <Alert severity="info" sx={{ mb: 1 }}>
            Your address book is empty. Ask a family admin to approve people for you.{" "}
            <Link href="/settings/book">Address book</Link>
          </Alert>
        )}
        <Autocomplete<ChatOption, false, false, boolean>
          freeSolo={isFamilyAdmin}
          autoHighlight
          openOnFocus
          options={options}
          value={value}
          groupBy={(o) => o.group}
          getOptionLabel={(o) => (typeof o === "string" ? o : o.label)}
          getOptionDisabled={(o) => typeof o !== "string" && o.disabled}
          isOptionEqualToValue={(a, b) =>
            typeof a !== "string" && typeof b !== "string" ? a.alias === b.alias : a === b
          }
          noOptionsText="No one in your address book matches"
          inputValue={inputValue}
          onInputChange={(_, v) => {
            setInputValue(v);
            setNotice(null);
          }}
          onChange={(_, v) => {
            setValue(v);
            if (!v) return;
            if (typeof v === "string") handleSubmit(v);
            else go(v.alias);
          }}
          filterOptions={filter}
          renderOption={(props, o) => {
            const { key, ...rest } = props;
            return (
              <li key={key} {...rest}>
                {o.external && o.phone && <PhoneIcon fontSize="small" sx={{ mr: 1 }} />}
                {o.label}
              </li>
            );
          }}
          renderInput={(params) => (
            <TextField
              {...params}
              autoFocus
              margin="dense"
              label={isFamilyAdmin ? "Name or @alias" : "Search your address book"}
            />
          )}
        />
        {notice && (
          <Alert severity="info" sx={{ mt: 1 }}>
            {notice}
          </Alert>
        )}
      </DialogContent>
      <DialogActions>
        <Button onClick={handleClose}>Cancel</Button>
        <Button variant="contained" disabled={!canOpen} onClick={open_}>
          Open
        </Button>
      </DialogActions>
    </Dialog>
  );
}

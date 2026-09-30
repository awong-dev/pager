"use client";

/** `/chat`'s "New chat" dialog -- docs/FAMILIES_DESIGN.md §5.2 "New chat":
 * one `Autocomplete` field ("Name, @alias or phone number"), suggestions
 * from `useDirectory()` grouped Family / People / Numbers, plus a live
 * "Text +1 555 123 4567" suggestion whenever the typed text parses as a
 * phone number. Selecting or submitting an option only navigates to
 * `/chat/{alias}` (or `/chat/{digits}` for a number) -- the conversation
 * itself is created by that thread's first send (docs/FAMILIES_TASKS.md
 * 3.5), never by this dialog.
 */

import { useMemo, useState } from "react";
import { useRouter } from "next/navigation";
import Autocomplete from "@mui/material/Autocomplete";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import TextField from "@mui/material/TextField";

import { useAuth } from "@/lib/auth-context";
import { useDirectory } from "@/lib/directory";
import { useFamily } from "@/lib/family-context";

type ChatOptionGroup = "Family" | "People" | "Numbers";

interface ChatOption {
  /** Route alias -- what `/chat/{alias}` gets pushed to (E.164 digits for
   * a number, the person/group alias otherwise). */
  alias: string;
  label: string;
  group: ChatOptionGroup;
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

interface NewChatDialogProps {
  open: boolean;
  onClose: () => void;
}

export default function NewChatDialog({ open, onClose }: NewChatDialogProps) {
  const router = useRouter();
  const { me } = useAuth();
  const { familyId } = useFamily();
  const { contacts, externals, byUid } = useDirectory();
  const [inputValue, setInputValue] = useState("");

  // Family / People / Numbers, per §5.2 -- a person is "Family" when their
  // own `familyId` matches the scope in view, "People" otherwise (an
  // allow-edge peer or a fellow conversation participant in another
  // family); externals are always "Numbers".
  const options = useMemo<ChatOption[]>(() => {
    const rows: ChatOption[] = [];
    for (const c of contacts) {
      if (c.uid === me?.uid) continue;
      const entry = byUid(c.uid);
      rows.push({
        alias: c.alias,
        label: `${c.displayName} (@${c.alias})`,
        group: entry?.familyId === familyId ? "Family" : "People",
      });
    }
    for (const e of externals) {
      const phone = e.phone ?? e.alias;
      rows.push({ alias: e.alias, label: `${e.displayName} (${formatPhoneDigits(phone)})`, group: "Numbers" });
    }
    return rows;
  }, [contacts, externals, byUid, familyId, me?.uid]);

  const phoneOption = useMemo<ChatOption | null>(() => {
    const digits = parsePhoneDigits(inputValue);
    return digits ? { alias: digits, label: `Text ${formatPhoneDigits(digits)}`, group: "Numbers" } : null;
  }, [inputValue]);

  const allOptions = useMemo(
    () => (phoneOption ? [...options, phoneOption] : options),
    [options, phoneOption]
  );

  function reset() {
    setInputValue("");
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

  // An alias typed with or without its leading `@` navigates the same way;
  // a phone-shaped value navigates by its E.164 digits instead (§5.2).
  function handleSubmit(value: string) {
    const trimmed = value.trim();
    if (!trimmed) return;
    const digits = parsePhoneDigits(trimmed);
    go(digits ?? trimmed.replace(/^@/, ""));
  }

  return (
    <Dialog open={open} onClose={handleClose} fullWidth maxWidth="xs">
      <DialogTitle>New chat</DialogTitle>
      <DialogContent>
        <Autocomplete
          freeSolo
          autoHighlight
          options={allOptions}
          groupBy={(o) => o.group}
          getOptionLabel={(o) => (typeof o === "string" ? o : o.label)}
          inputValue={inputValue}
          onInputChange={(_, value) => setInputValue(value)}
          onChange={(_, value) => {
            if (!value) return;
            if (typeof value === "string") {
              handleSubmit(value);
            } else {
              go(value.alias);
            }
          }}
          filterOptions={(opts, state) => {
            const q = state.inputValue.trim().toLowerCase();
            return opts.filter(
              (o) => o === phoneOption || !q || o.label.toLowerCase().includes(q) || o.alias.toLowerCase().includes(q)
            );
          }}
          renderInput={(params) => (
            <TextField {...params} autoFocus margin="dense" label="Name, @alias or phone number" />
          )}
        />
      </DialogContent>
      <DialogActions>
        <Button onClick={handleClose}>Cancel</Button>
        <Button variant="contained" onClick={() => handleSubmit(inputValue)}>
          Open
        </Button>
      </DialogActions>
    </Dialog>
  );
}

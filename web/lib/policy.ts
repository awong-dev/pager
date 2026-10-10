/**
 * Conversation policy code/label/description tables -- docs/FAMILIES_DESIGN.md
 * §2 (both tables). Pure module (no React/MUI) so it can be unit tested
 * later per docs/FAMILIES_TASKS.md 3.4; `lib/types.ts`'s `PolicyDoc.{out,in}`
 * stays a plain `string` since this file has no relay-side counterpart to
 * import the literal unions from (same convention as that file's comment).
 */

export interface PolicyOption {
  code: string;
  label: string;
  /** One-line description shown under the radio button. */
  description: string;
  /** Not offered as a new choice; still shown when it is the current value. */
  hidden?: boolean;
}

// docs/FAMILIES_DESIGN.md §2 "Outbound" table: `users.policy.out`, "who @kid
// can start or continue a chat with". "Numbers" are external SMS contacts
// reached through the member's bridge phone (docs/BRIDGE_PHONE_DESIGN.md);
// the number options were hidden from 7 Oct 2026 (no relay SMS) until the
// first bridge phone went live on 9 Oct 2026.
export const OUTBOUND_POLICIES: PolicyOption[] = [
  {
    code: "open",
    label: "Open",
    description: "Message any person or any number. Admins are alerted when a new conversation starts.",
  },
  { code: "people", label: "People", description: "Only approved people, no numbers." },
  { code: "people_sms", label: "People + Numbers", description: "Approved people and approved numbers." },
  { code: "sms", label: "Numbers", description: "Only approved numbers, no people." },
  { code: "any_sms", label: "Any number", description: "No people, but any phone number." },
];

// docs/FAMILIES_DESIGN.md §2 "Inbound" table: `users.policy.in`, "who may
// reach @kid". An unknown number is never delivered (docs/RELAY_SMS_DESIGN.md,
// owner 8 Oct 2026): it is held and raises an alert; approving it there
// needs a policy whose numbers column is not "none".
export const INBOUND_POLICIES: PolicyOption[] = [
  { code: "any", label: "Anyone", description: "Any person or any approved number may reach them." },
  { code: "people", label: "People", description: "Only approved people may reach them." },
  {
    code: "people_sms",
    label: "People + Numbers",
    description: "Approved people and approved numbers may reach them.",
  },
  { code: "sms", label: "Numbers", description: "Only approved numbers may reach them, no people." },
  {
    code: "any_sms",
    label: "Any number",
    description: "No people; approved numbers reach them and unknown numbers are held for your approval.",
  },
];

function labelOf(options: PolicyOption[], code: string): string {
  return options.find((o) => o.code === code)?.label ?? code;
}

export function outboundLabel(code: string): string {
  return labelOf(OUTBOUND_POLICIES, code);
}

export function inboundLabel(code: string): string {
  return labelOf(INBOUND_POLICIES, code);
}

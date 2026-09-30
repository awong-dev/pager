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
}

// docs/FAMILIES_DESIGN.md §2 "Outbound" table: `users.policy.out`, "who @kid
// can start or continue a chat with".
export const OUTBOUND_POLICIES: PolicyOption[] = [
  {
    code: "open",
    label: "Open",
    description: "Message any person or any number. Admins are alerted when a new conversation starts.",
  },
  {
    code: "people",
    label: "People",
    description: "Only approved people, no numbers.",
  },
  {
    code: "people_sms",
    label: "People + Numbers",
    description: "Approved people and approved numbers.",
  },
  {
    code: "sms",
    label: "Numbers",
    description: "Only approved numbers, no people.",
  },
  {
    code: "any_sms",
    label: "Any number",
    description: "No people, but any phone number.",
  },
];

// docs/FAMILIES_DESIGN.md §2 "Inbound" table: `users.policy.in`, "who may
// reach @kid".
export const INBOUND_POLICIES: PolicyOption[] = [
  {
    code: "any",
    label: "Anyone",
    description: "Any person or any number may reach them.",
  },
  {
    code: "people",
    label: "People",
    description: "Only approved people may reach them.",
  },
  {
    code: "people_sms",
    label: "People + Numbers",
    description: "Approved people and approved numbers may reach them.",
  },
  {
    code: "sms",
    label: "Numbers",
    description: "Only approved numbers may reach them, no people.",
  },
  {
    code: "any_sms",
    label: "Any number",
    description: "No people, but any phone number -- unrecognised texts are delivered and alert you.",
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

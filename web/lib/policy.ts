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

const PEOPLE_OUT = "Only approved people; only approved contacts on their pager.";

// docs/FAMILIES_DESIGN.md §2 "Outbound" table: `users.policy.out`, "who @kid
// can start or continue a chat with". The relay sends no SMS
// (docs/V02_DESIGN.md §6): "contacts" are the pager's own SMS list. The
// `hidden` codes duplicate a visible one for people and stay only so stored
// values still get a label.
export const OUTBOUND_POLICIES: PolicyOption[] = [
  {
    code: "open",
    label: "Open",
    description:
      "Message any person; every family contact is on their pager. Admins are alerted when a new conversation starts.",
  },
  { code: "people", label: "People", description: PEOPLE_OUT },
  { code: "people_sms", label: "People", description: PEOPLE_OUT, hidden: true },
  { code: "sms", label: "No people", description: "No people; only approved contacts on their pager." },
  {
    code: "any_sms",
    label: "No people",
    description: "No people; every family contact is on their pager.",
    hidden: true,
  },
];

// docs/FAMILIES_DESIGN.md §2 "Inbound" table: `users.policy.in`, "who may
// reach @kid".
export const INBOUND_POLICIES: PolicyOption[] = [
  { code: "any", label: "Anyone", description: "Any person may reach them." },
  { code: "people", label: "People", description: "Only approved people may reach them." },
  { code: "people_sms", label: "People", description: "Only approved people may reach them.", hidden: true },
  { code: "sms", label: "No people", description: "No person may reach them." },
  { code: "any_sms", label: "No people", description: "No person may reach them.", hidden: true },
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

/** Server-component shell for `/chat/view/[key]` -- same static-export
 * placeholder-shell design as `/chat/[alias]` (see
 * `app/chat/[alias]/ThreadPageClient.tsx`'s module docstring). Only
 * `generateStaticParams`/`dynamicParams` live here; all real behaviour is
 * `MonitorThreadClient.tsx`, which reads the conversation key from
 * `usePathname()`.
 */

import MonitorThreadClient from "./MonitorThreadClient";

export function generateStaticParams() {
  return [{ key: "_" }];
}

// Literal `false` required by `output: 'export'` -- see
// `app/chat/[alias]/page.tsx` for the full explanation this mirrors.
export const dynamicParams = false;

export default function ChatViewPage() {
  return <MonitorThreadClient />;
}

/** Server-component shell for `/devices/[id]` -- same static-export
 * placeholder-shell design as `/chat/[alias]` (see
 * `app/chat/[alias]/ThreadPageClient.tsx`'s module docstring). Only
 * `generateStaticParams`/`dynamicParams` live here; all real behaviour is
 * `DevicePageClient.tsx`, which reads the id from `usePathname()`.
 */

import DevicePageClient from "./DevicePageClient";

export function generateStaticParams() {
  return [{ id: "_" }];
}

// Literal `false` required by `output: 'export'` -- see
// `app/chat/[alias]/page.tsx` for the full explanation this mirrors.
export const dynamicParams = false;

export default function DeviceDetailPage() {
  return <DevicePageClient />;
}

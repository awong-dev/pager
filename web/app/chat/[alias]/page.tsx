/** Server-component shell for `/chat/[alias]` -- `generateStaticParams`
 * (and the `output: 'export'`-required `dynamicParams`) can only be
 * exported from a non-`"use client"` file, so this thin file exists purely
 * to declare them; all real behaviour is `ThreadPageClient.tsx`. See that
 * file's module docstring for the static-export placeholder-shell design
 * this implements.
 */

import ThreadPageClient from "./ThreadPageClient";

export function generateStaticParams() {
  return [{ alias: "_" }];
}

// Must stay a literal `false`: `output: 'export'` requires it (Next rejects
// `dynamicParams: true` under export), and Next parses this statically, so
// it cannot be an expression. The only pre-rendered page is the `/chat/_`
// shell above; Firebase Hosting rewrites every real `/chat/<alias>` onto it
// (`web/firebase.json`), and `web/next.config.ts` reproduces that rewrite
// for `next dev`, which has no such host in front of it.
export const dynamicParams = false;

export default function ChatThreadPage() {
  return <ThreadPageClient />;
}

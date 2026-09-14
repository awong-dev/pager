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
export const dynamicParams = false;

export default function ChatThreadPage() {
  return <ThreadPageClient />;
}

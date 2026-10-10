import type { UserKind } from "./types";

// Externals (SMS contacts, bridged Google Chat/WhatsApp) get a hash alias
// (`x<11 hex>` / `c<11 hex>`) that is a routing key, not something to show.
const HASH_ALIAS = /^[xc][0-9a-f]{11}$/;

export function isHashAlias(alias: string | undefined): boolean {
  return !!alias && HASH_ALIAS.test(alias);
}

/** Primary label for a peer: persons keep `@alias`; externals / hash aliases
 * show the contact's name only (falling back to the alias if no name). */
export function peerLabel(p: { alias?: string; displayName?: string | null; kind?: UserKind }): string {
  const alias = p.alias ?? "";
  const name = p.displayName?.trim();
  if (p.kind === "external" || isHashAlias(alias)) return name || alias || "someone";
  return alias ? `@${alias}` : name || "someone";
}

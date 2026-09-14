"use client";

/** `/` -- docs/SERVER_PLAN.md §7.2: "redirects to /chat". No server-side
 * redirect exists in an `output: 'export'` build, so this is a client-side
 * one, same as every other route in this app. */

import { useRouter } from "next/navigation";
import { useEffect } from "react";

export default function Home() {
  const router = useRouter();
  useEffect(() => {
    router.replace("/chat");
  }, [router]);
  return null;
}

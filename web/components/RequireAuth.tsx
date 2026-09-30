"use client";

import { useRouter } from "next/navigation";
import { type ReactNode, useEffect } from "react";
import Box from "@mui/material/Box";
import CircularProgress from "@mui/material/CircularProgress";

import { useAuth } from "@/lib/auth-context";

function LoadingScreen() {
  return (
    <Box sx={{ display: "flex", justifyContent: "center", alignItems: "center", height: "60vh" }}>
      <CircularProgress />
    </Box>
  );
}

/** Client-side route guard -- docs/SERVER_PLAN.md §7.3: "Admin routes are
 * hidden unless the `admin` claim is present ... the client is not the
 * gate" (the relay/rules enforce the real boundary; this is UX only). Every
 * authenticated page wraps its content in this.
 *
 * `requireRole: 'admin'` accepts a family admin *or* super (a super can do
 * everything a family admin can, scoped by `useFamily()`);
 * `requireRole: 'super'` accepts only super. `requireAdmin` is a deprecated
 * alias for `requireRole="admin"`, kept so pages this task doesn't touch
 * keep compiling (docs/FAMILIES_TASKS.md 1.7). */
export default function RequireAuth({
  children,
  requireAdmin = false,
  requireRole,
}: {
  children: ReactNode;
  /** @deprecated use `requireRole="admin"` instead. */
  requireAdmin?: boolean;
  requireRole?: "admin" | "super";
}) {
  const { status, isFamilyAdmin, isSuper } = useAuth();
  const router = useRouter();
  const effectiveRole = requireRole ?? (requireAdmin ? "admin" : undefined);
  const allowed =
    effectiveRole === undefined ? true : effectiveRole === "super" ? isSuper : isFamilyAdmin;

  useEffect(() => {
    if (status === "signed-out" || status === "not-registered") {
      router.replace("/login");
    } else if (status === "signed-in" && !allowed) {
      router.replace("/chat");
    }
  }, [status, allowed, router]);

  if (status !== "signed-in" || !allowed) {
    return <LoadingScreen />;
  }

  return <>{children}</>;
}

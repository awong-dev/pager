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
 * authenticated page wraps its content in this. */
export default function RequireAuth({
  children,
  requireAdmin = false,
}: {
  children: ReactNode;
  requireAdmin?: boolean;
}) {
  const { status, isAdmin } = useAuth();
  const router = useRouter();

  useEffect(() => {
    if (status === "signed-out" || status === "not-registered") {
      router.replace("/login");
    } else if (status === "signed-in" && requireAdmin && !isAdmin) {
      router.replace("/chat");
    }
  }, [status, isAdmin, requireAdmin, router]);

  if (status !== "signed-in" || (requireAdmin && !isAdmin)) {
    return <LoadingScreen />;
  }

  return <>{children}</>;
}

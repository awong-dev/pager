"use client";

import CssBaseline from "@mui/material/CssBaseline";
import { ThemeProvider } from "@mui/material/styles";
import { AppRouterCacheProvider } from "@mui/material-nextjs/v16-appRouter";
import type { ReactNode } from "react";

import { AuthProvider } from "@/lib/auth-context";
import { DirectoryProvider } from "@/lib/directory";
import theme from "@/lib/theme";

/** Every provider the app needs, in one place, mounted once from the root
 * layout -- docs/SERVER_PLAN.md §7.1: no state library beyond React context. */
export default function Providers({ children }: { children: ReactNode }) {
  return (
    <AppRouterCacheProvider options={{ key: "mui" }}>
      <ThemeProvider theme={theme}>
        <CssBaseline />
        <AuthProvider>
          <DirectoryProvider>{children}</DirectoryProvider>
        </AuthProvider>
      </ThemeProvider>
    </AppRouterCacheProvider>
  );
}

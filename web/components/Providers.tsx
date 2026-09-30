"use client";

import CssBaseline from "@mui/material/CssBaseline";
import { ThemeProvider } from "@mui/material/styles";
import { AppRouterCacheProvider } from "@mui/material-nextjs/v16-appRouter";
import type { ReactNode } from "react";

import { AuthProvider } from "@/lib/auth-context";
import { DirectoryProvider } from "@/lib/directory";
import { FamilyProvider } from "@/lib/family-context";
import theme from "@/lib/theme";

/** Every provider the app needs, in one place, mounted once from the root
 * layout -- docs/SERVER_PLAN.md §7.1: no state library beyond React context.
 * `FamilyProvider` sits inside `AuthProvider` (it needs `useAuth()` for
 * `role`/`familyId`) and outside `DirectoryProvider` (docs/FAMILIES_DESIGN.md
 * §5.1: the directory rewrite in task 1.9 scopes by family). */
export default function Providers({ children }: { children: ReactNode }) {
  return (
    <AppRouterCacheProvider options={{ key: "mui" }}>
      <ThemeProvider theme={theme}>
        <CssBaseline />
        <AuthProvider>
          <FamilyProvider>
            <DirectoryProvider>{children}</DirectoryProvider>
          </FamilyProvider>
        </AuthProvider>
      </ThemeProvider>
    </AppRouterCacheProvider>
  );
}

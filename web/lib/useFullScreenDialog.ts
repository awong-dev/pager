"use client";

import { useMediaQuery, useTheme } from "@mui/material";

/** Dialogs go full screen below the `sm` breakpoint (phones). Spread the
 * result onto a `<Dialog>`: `<Dialog fullWidth maxWidth="sm" {...useFullScreenDialog()}>`
 * or use the returned boolean directly. */
export function useFullScreenDialog(): boolean {
  const theme = useTheme();
  return useMediaQuery(theme.breakpoints.down("sm"));
}

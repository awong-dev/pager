import type { SxProps, Theme } from "@mui/material/styles";

/** Full-height column for the chat thread pages. Desktop keeps the old
 * `100vh - 140px` (64 px AppBar + 24 px container padding x2 + slack); xs
 * subtracts the 56 px mobile AppBar + 16 px padding x2 + slack. `100vh` is
 * the fallback; browsers that know `dvh` use it so iOS Safari's collapsing
 * toolbars don't push the composer off screen. */
export const threadHeightSx: SxProps<Theme> = {
  height: { xs: "calc(100vh - 96px)", md: "calc(100vh - 140px)" },
  "@supports (height: 100dvh)": {
    height: { xs: "calc(100dvh - 96px)", md: "calc(100dvh - 140px)" },
  },
};

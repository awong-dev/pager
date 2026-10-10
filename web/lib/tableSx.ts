import type { SxProps, Theme } from "@mui/material/styles";

/** `sx` for a `TableContainer`: horizontal scroll (so a wide table can never
 * widen the page) and the given 1-based columns hidden below `md`, on both the
 * header and body cells. Used instead of per-cell `display` props so the
 * header/body pair can't drift apart. */
export function responsiveTableSx(hideOnXs: number[] = [], stickyFirst = false): SxProps<Theme> {
  const hidden: Record<string, unknown> = {};
  for (const n of hideOnXs) {
    hidden[`& tr > :nth-child(${n})`] = { display: { xs: "none", md: "table-cell" } };
  }
  if (stickyFirst) {
    hidden["& tr > :first-child"] = { position: "sticky", left: 0, zIndex: 1, bgcolor: "background.paper" };
  }
  return { overflowX: "auto", maxWidth: "100%", ...hidden } as SxProps<Theme>;
}

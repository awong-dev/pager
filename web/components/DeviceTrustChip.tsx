import Chip from "@mui/material/Chip";
import Tooltip from "@mui/material/Tooltip";
import LockIcon from "@mui/icons-material/Lock";
import LockOpenIcon from "@mui/icons-material/LockOpen";

import { trustChipInfo } from "@/lib/deviceTrust";
import type { TlsState } from "@/lib/types";

/** `/admin/devices`' per-device CA trust chip -- docs/V02_DESIGN.md §4.3.
 * Renders nothing when `tls` is absent (older firmware), per spec. */
export default function DeviceTrustChip({
  tls,
  caFp,
}: {
  tls?: TlsState | null;
  caFp?: string | null;
}) {
  const info = trustChipInfo(tls, caFp);
  if (!info) return null;
  const icon =
    info.variant === "pinned" ? (
      <LockIcon fontSize="small" />
    ) : info.variant === "broken" ? (
      <LockOpenIcon fontSize="small" />
    ) : undefined;
  return (
    <Tooltip title={info.tooltip}>
      <Chip
        size="small"
        label={info.label}
        color={info.color}
        icon={icon}
        variant={info.variant === "unpinned" ? "outlined" : "filled"}
      />
    </Tooltip>
  );
}

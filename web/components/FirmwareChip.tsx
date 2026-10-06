import Chip from "@mui/material/Chip";
import Stack from "@mui/material/Stack";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";

import { otaStateLabel } from "@/lib/firmware";
import type { DeviceStatusDoc } from "@/lib/types";

/** Firmware version chip plus the OTA job line -- docs/OTA_DESIGN.md §5. */
export default function FirmwareChip({ status }: { status?: DeviceStatusDoc | null }) {
  const job = otaStateLabel(status);
  const bad = status?.otaState === "fail" || status?.otaState === "rb";
  return (
    <Stack spacing={0.5} sx={{ alignItems: "flex-start" }}>
      <Tooltip title={status?.img ? `image ${status.img}` : "image id not reported"}>
        <Chip size="small" variant="outlined" label={status?.fw || "?"} />
      </Tooltip>
      {job && (
        <Typography variant="caption" color={bad ? "error" : "text.secondary"}>
          {job}
        </Typography>
      )}
    </Stack>
  );
}

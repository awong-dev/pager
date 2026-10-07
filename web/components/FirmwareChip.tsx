import Chip from "@mui/material/Chip";
import Stack from "@mui/material/Stack";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";

import { otaStateLabel, type FirmwareBuild } from "@/lib/firmware";
import type { DeviceStatusDoc } from "@/lib/types";

/** Firmware version chip plus the OTA job line -- docs/OTA_DESIGN.md §5. */
export default function FirmwareChip({
  status,
  newest,
}: {
  status?: DeviceStatusDoc | null;
  newest?: FirmwareBuild;
}) {
  let job = otaStateLabel(status);
  const bad = status?.otaState === "fail" || status?.otaState === "rb";
  let warn = false;
  // A finished job ("Updated") says nothing about newer builds: compare the
  // running image with the newest published one instead.
  if (newest && (status?.otaState === "ok" || !status?.otaState)) {
    if (status?.img && status.img === newest.id16) {
      job = "Up to date";
    } else if (status?.img) {
      job = `Update available: ${newest.version}`;
      warn = true;
    }
  }
  return (
    <Stack spacing={0.5} sx={{ alignItems: "flex-start" }}>
      <Tooltip title={status?.img ? `image ${status.img}` : "image id not reported"}>
        <Chip size="small" variant="outlined" label={status?.fw || "?"} />
      </Tooltip>
      {job && (
        <Typography variant="caption" color={bad ? "error" : warn ? "warning.main" : "text.secondary"}>
          {job}
        </Typography>
      )}
    </Stack>
  );
}

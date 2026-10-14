import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import { isAssetJob, useAssetBuilds } from "@/lib/assets";
import { otaStateLabel, type FirmwareScope } from "@/lib/firmware";
import type { DeviceDoc } from "@/lib/types";

/** `Fonts: factory | <label> (<id16>) | —` plus the job line while a font job exists -- docs/ASSETS_OTA_DESIGN.md §3. */
export default function FontsChip({
  device,
  scope,
}: {
  device: DeviceDoc & { id: string };
  scope: FirmwareScope;
}) {
  const src = device.status?.assetsSrc ?? null;
  const id16 = device.status?.assetsId ?? null;
  const builds = useAssetBuilds(src === 2 && !!id16, scope, device.id);
  let text = "Fonts: —";
  if (src === 1) {
    text = "Fonts: factory";
  } else if (src === 2) {
    const b = builds.find((x) => x.id16 === id16?.toLowerCase());
    text = `Fonts: ${b?.label ? `${b.label} (${id16})` : (id16 ?? "OTA")}`;
  }
  const job = isAssetJob(device) ? otaStateLabel(device.status) : null;
  const bad = device.status?.otaState === "fail";
  return (
    <Stack spacing={0.5} sx={{ alignItems: "flex-start" }}>
      <Typography variant="caption" color="text.secondary">
        {text}
      </Typography>
      {job && (
        <Typography variant="caption" color={bad ? "error" : "text.secondary"}>
          {job.replace(/^Update /, "Font update ")}
        </Typography>
      )}
    </Stack>
  );
}

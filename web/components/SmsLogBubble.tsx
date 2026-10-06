"use client";

/** One pager-modem SMS audit row (`GET /api/devices/{id}/sms-log`) rendered
 * as a chat bubble in an SMS contact's thread: `dir: "out"` = from the pager
 * (right, like the owner's own messages), `"in"` = from the contact (left).
 * Malformed rows show the "malformed" chip, the reason in italics and a
 * "raw" toggle for the hex. */

import { useState } from "react";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Stack from "@mui/material/Stack";
import Typography from "@mui/material/Typography";

import { statusColor, type SmsLogEntry } from "@/lib/smsContacts";
import { formatClock } from "@/lib/time";

export default function SmsLogBubble({ entry: e }: { entry: SmsLogEntry }) {
  const [showRaw, setShowRaw] = useState(false);
  const bad = e.malformed === true;
  const mine = e.dir === "out";
  const failed = e.st === "failed";
  return (
    <Stack sx={{ alignItems: mine ? "flex-end" : "flex-start", my: 0.75 }}>
      <Box
        sx={{
          px: 1.5,
          py: 1,
          maxWidth: "80%",
          borderRadius: 2,
          bgcolor: bad ? "warning.light" : mine ? "primary.main" : "grey.100",
          color: bad ? "text.primary" : mine ? "primary.contrastText" : "text.primary",
        }}
      >
        {bad ? (
          <Typography variant="body2" sx={{ fontStyle: "italic", wordBreak: "break-word" }}>
            {e.reason ?? "—"}
          </Typography>
        ) : (
          <Typography variant="body1" sx={{ whiteSpace: "pre-wrap", wordBreak: "break-word" }}>
            {e.body ?? "—"}
          </Typography>
        )}
        {bad && e.rawHex && (
          <>
            <Button size="small" sx={{ minWidth: 0, p: 0, textTransform: "none" }} onClick={() => setShowRaw((v) => !v)}>
              raw
            </Button>
            {showRaw && (
              <Typography component="div" variant="caption" sx={{ fontFamily: "monospace", wordBreak: "break-all" }}>
                {e.rawHex}
              </Typography>
            )}
          </>
        )}
        <Stack direction="row" spacing={0.5} sx={{ alignItems: "center", mt: 0.5 }}>
          <Chip size="small" variant="outlined" label="modem" sx={{ height: 18, color: "inherit", borderColor: "currentColor" }} />
          {bad && <Chip size="small" color="error" label="malformed" sx={{ height: 18 }} />}
          {e.st && (
            <Chip
              size="small"
              label={e.st}
              color={failed ? "error" : statusColor(e.st)}
              sx={{ height: 18 }}
            />
          )}
          <Typography variant="caption" sx={{ opacity: 0.7 }}>
            {formatClock((e.smsTs || e.ts) * 1000)}
          </Typography>
        </Stack>
      </Box>
    </Stack>
  );
}

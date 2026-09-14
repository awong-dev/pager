"use client";

/** `/login` -- docs/SERVER_PLAN.md §7.3: single email-or-phone input, hand
 * built with MUI over Firebase Auth. Email uses the passwordless email-link
 * flow (`sendSignInLinkToEmail` -> the link lands back here with
 * `?finish=1` -> `signInWithEmailLink`); phone uses invisible reCAPTCHA +
 * `signInWithPhoneNumber` -> a 6-digit code. After sign-in,
 * `lib/auth-context.tsx` calls `GET /api/me` itself and exposes a 403 as
 * `notRegisteredMessage`, which this page renders and treats as staying on
 * `/login` (the account was already signed out by the context).
 */

import {
  type ConfirmationResult,
  RecaptchaVerifier,
  isSignInWithEmailLink,
  sendSignInLinkToEmail,
  signInWithEmailLink,
  signInWithPhoneNumber,
} from "firebase/auth";
import { useRouter } from "next/navigation";
import { useEffect, useRef, useState } from "react";
import Alert from "@mui/material/Alert";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Paper from "@mui/material/Paper";
import Stack from "@mui/material/Stack";
import TextField from "@mui/material/TextField";
import Typography from "@mui/material/Typography";

import { useAuth } from "@/lib/auth-context";
import { getFirebaseAuth } from "@/lib/firebase";

const EMAIL_LINK_STORAGE_KEY = "pager.signInEmail";
const E164_RE = /^\+[1-9]\d{1,14}$/;

type Mode = "enter-identifier" | "email-sent" | "enter-code" | "completing";

export default function LoginPage() {
  const { status, notRegisteredMessage } = useAuth();
  const router = useRouter();

  const [mode, setMode] = useState<Mode>("enter-identifier");
  const [identifier, setIdentifier] = useState("");
  const [code, setCode] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const confirmationRef = useRef<ConfirmationResult | null>(null);
  const recaptchaContainerRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (status === "signed-in") {
      router.replace("/chat");
    }
  }, [status, router]);

  // Completing an email-link sign-in: the link lands back on this exact
  // page with `?finish=1`.
  useEffect(() => {
    // The whole body runs inside an async IIFE (rather than setState calls
    // sitting directly in the effect body) so every `setError`/`setMode`
    // call happens inside a callback, not synchronously during the effect's
    // commit -- same rule `lib/directory.tsx` documents at more length.
    void (async () => {
      if (typeof window === "undefined") return;
      const params = new URLSearchParams(window.location.search);
      if (params.get("finish") !== "1") return;

      const auth = getFirebaseAuth();
      if (!isSignInWithEmailLink(auth, window.location.href)) {
        setError("This sign-in link is invalid or has expired. Request a new one below.");
        return;
      }
      let email = window.localStorage.getItem(EMAIL_LINK_STORAGE_KEY);
      if (!email) {
        email = window.prompt("Confirm the email address you signed in with:");
      }
      if (!email) {
        setError("Sign-in needs the email address you started with.");
        return;
      }
      setMode("completing");
      try {
        await signInWithEmailLink(auth, email, window.location.href);
        window.localStorage.removeItem(EMAIL_LINK_STORAGE_KEY);
        // AuthProvider's onAuthStateChanged listener picks this up and
        // redirects once `GET /api/me` resolves.
      } catch (err) {
        setMode("enter-identifier");
        setError(err instanceof Error ? err.message : "Sign-in failed.");
      }
    })();
  }, []);

  async function handleSubmitIdentifier() {
    setError(null);
    const value = identifier.trim();
    if (!value) return;
    setBusy(true);
    try {
      if (value.includes("@")) {
        const auth = getFirebaseAuth();
        const url = new URL(window.location.href);
        url.search = "?finish=1";
        await sendSignInLinkToEmail(auth, value, {
          url: url.toString(),
          handleCodeInApp: true,
        });
        window.localStorage.setItem(EMAIL_LINK_STORAGE_KEY, value);
        setMode("email-sent");
      } else if (E164_RE.test(value)) {
        const auth = getFirebaseAuth();
        if (!recaptchaContainerRef.current) {
          throw new Error("reCAPTCHA container missing");
        }
        const verifier = new RecaptchaVerifier(auth, recaptchaContainerRef.current, {
          size: "invisible",
        });
        confirmationRef.current = await signInWithPhoneNumber(auth, value, verifier);
        setMode("enter-code");
      } else {
        setError("Enter a valid email address or a phone number in +1XXXXXXXXXX (E.164) format.");
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : "Sign-in failed.");
    } finally {
      setBusy(false);
    }
  }

  async function handleSubmitCode() {
    if (!confirmationRef.current) return;
    setError(null);
    setBusy(true);
    try {
      await confirmationRef.current.confirm(code.trim());
      // AuthProvider handles the rest.
    } catch (err) {
      setError(err instanceof Error ? err.message : "That code did not match.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Box sx={{ display: "flex", justifyContent: "center", alignItems: "center", minHeight: "100vh", p: 2 }}>
      <Paper elevation={2} sx={{ p: 4, maxWidth: 420, width: "100%" }}>
        <Typography variant="h5" gutterBottom>
          Sign in to Pager
        </Typography>

        {notRegisteredMessage && <Alert severity="warning" sx={{ mb: 2 }}>{notRegisteredMessage}</Alert>}
        {error && <Alert severity="error" sx={{ mb: 2 }}>{error}</Alert>}

        {mode === "completing" && (
          <Stack sx={{ alignItems: "center", py: 4 }}>
            <CircularProgress />
            <Typography sx={{ mt: 2 }}>Completing sign-in...</Typography>
          </Stack>
        )}

        {mode === "enter-identifier" && (
          <Stack spacing={2}>
            <TextField
              label="Email or phone (+1XXXXXXXXXX)"
              value={identifier}
              onChange={(e) => setIdentifier(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && void handleSubmitIdentifier()}
              autoFocus
              fullWidth
            />
            <Button variant="contained" disabled={busy} onClick={() => void handleSubmitIdentifier()}>
              {busy ? "Sending..." : "Continue"}
            </Button>
          </Stack>
        )}

        {mode === "email-sent" && (
          <Alert severity="success">
            Check your email for a sign-in link. Opening it on this device finishes sign-in.
          </Alert>
        )}

        {mode === "enter-code" && (
          <Stack spacing={2}>
            <Typography variant="body2">Enter the 6-digit code sent by SMS.</Typography>
            <TextField
              label="Code"
              value={code}
              onChange={(e) => setCode(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && void handleSubmitCode()}
              autoFocus
              fullWidth
            />
            <Button variant="contained" disabled={busy} onClick={() => void handleSubmitCode()}>
              {busy ? "Verifying..." : "Verify"}
            </Button>
          </Stack>
        )}

        {/* Invisible reCAPTCHA anchor -- never visible, required by
            `signInWithPhoneNumber`. */}
        <div ref={recaptchaContainerRef} />
      </Paper>
    </Box>
  );
}

/** Browser Notifications + FCM registration -- docs/SERVER_PLAN.md §7.6.
 * Permission is only ever requested from `/settings/notifications`, never on
 * first load (per §7.3/§7.6's explicit rule), so nothing in this module
 * calls `Notification.requestPermission()` on import or on mount elsewhere.
 */

import { getToken } from "firebase/messaging";
import { getMessagingIfSupported } from "./firebase-messaging";

export function notificationsSupported(): boolean {
  return typeof window !== "undefined" && "Notification" in window;
}

export function notificationPermission(): NotificationPermission | "unsupported" {
  if (!notificationsSupported()) return "unsupported";
  return Notification.permission;
}

/** Registers the service worker and mints an FCM token -- caller is
 * responsible for POSTing it to `/api/me/push-tokens` (kept out of this
 * module so it stays a thin browser-API wrapper, not an API-call site). */
export async function registerForPush(): Promise<string | null> {
  if (!notificationsSupported()) return null;
  const permission = await Notification.requestPermission();
  if (permission !== "granted") return null;

  const messaging = await getMessagingIfSupported();
  if (!messaging) return null;

  const registration = await navigator.serviceWorker.register("/firebase-messaging-sw.js");
  const vapidKey = process.env.NEXT_PUBLIC_FIREBASE_VAPID_KEY;
  const token = await getToken(messaging, {
    vapidKey,
    serviceWorkerRegistration: registration,
  });
  return token || null;
}

const SW_PATH = "/firebase-messaging-sw.js";

/** Chrome for Android throws "Illegal constructor" on `new Notification()`;
 * a service worker registration's `showNotification` works everywhere. The
 * worker's `notificationclick` handler reads `data.url`. Falls back to the
 * constructor only when no registration exists. */
async function showNotification(
  title: string,
  options: NotificationOptions,
  url: string
): Promise<void> {
  let registration: ServiceWorkerRegistration | undefined;
  if ("serviceWorker" in navigator) {
    registration = await navigator.serviceWorker.getRegistration(SW_PATH);
  }
  if (registration) {
    await registration.showNotification(title, { ...options, data: { url } });
    return;
  }
  const n = new Notification(title, options);
  n.onclick = () => {
    window.focus();
    // Full navigation, not `useRouter()` -- this runs outside React.
    window.location.href = url;
  };
}

/** A same-device, no-server-round-trip check that permission + display
 * actually work -- docs/SERVER_PLAN.md §7.2's "test button". There is no
 * relay endpoint that sends a real push on demand (§5.1 has none), so this
 * is deliberately local-only; see web/README.md. Rejects on failure. */
export async function showLocalTestNotification(): Promise<void> {
  if (notificationPermission() !== "granted") {
    throw new Error("Notification permission is not granted.");
  }
  await showNotification(
    "Pager test notification",
    {
      body: "If you can see this, browser notifications are working.",
      icon: "/icons/icon-192.png",
    },
    "/chat"
  );
}

/** Foreground rule, docs/SERVER_PLAN.md §7.6: show a Notification when the
 * tab isn't visible or the relevant thread isn't the one open. */
export async function showForegroundMessageNotification(
  alias: string,
  preview: string,
  /** Display label for the sender (lib/names.ts `peerLabel`); defaults to `@alias`. */
  label?: string
): Promise<void> {
  if (notificationPermission() !== "granted") return;
  await showNotification(
    `New message from ${label ?? `@${alias}`}`,
    {
      body: preview,
      icon: "/icons/icon-192.png",
      tag: `pager-thread-${alias}`,
    },
    `/chat/${encodeURIComponent(alias)}`
  );
}

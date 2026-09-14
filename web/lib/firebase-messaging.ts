import { type Messaging, getMessaging, isSupported } from "firebase/messaging";

import { getFirebaseApp } from "./firebase";

// `firebase/messaging` is not available during Next's build-time prerender
// pass (no `window`/`serviceWorker`) and not every browser supports FCM
// (`isSupported()` checks that) -- memoized so `lib/notifications.ts` can
// call this from any client effect without re-running the support probe.
let messagingPromise: Promise<Messaging | null> | null = null;

export function getMessagingIfSupported(): Promise<Messaging | null> {
  if (!messagingPromise) {
    messagingPromise = isSupported()
      .then((ok) => (ok ? getMessaging(getFirebaseApp()) : null))
      .catch(() => null);
  }
  return messagingPromise;
}

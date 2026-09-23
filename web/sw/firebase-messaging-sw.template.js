// Background push handler -- docs/SERVER_PLAN.md §7.6: FCM.
// `onBackgroundMessage` -> `showNotification`; `notificationclick` focuses an
// existing client already at the payload's `url`, or opens one. A service
// worker can't import the npm `firebase` package (no bundler runs here), so
// this uses the `-compat` scripts Firebase publishes for exactly this case,
// loaded from the CDN.
//
// This file is a TEMPLATE, not the served worker. `web/scripts/gen-sw.mjs`
// (wired as the `prebuild` npm script) substitutes the six
// `NEXT_PUBLIC_FIREBASE_*` placeholders below with the real values the app
// itself was built with and writes the result to
// `public/firebase-messaging-sw.js`, which is gitignored -- the static
// export must never ship a "demo" Firebase config again. Edit this template,
// never the generated file.
importScripts("https://www.gstatic.com/firebasejs/12.3.0/firebase-app-compat.js");
importScripts("https://www.gstatic.com/firebasejs/12.3.0/firebase-messaging-compat.js");

firebase.initializeApp({
  apiKey: "__NEXT_PUBLIC_FIREBASE_API_KEY__",
  authDomain: "__NEXT_PUBLIC_FIREBASE_AUTH_DOMAIN__",
  projectId: "__NEXT_PUBLIC_FIREBASE_PROJECT_ID__",
  storageBucket: "__NEXT_PUBLIC_FIREBASE_STORAGE_BUCKET__",
  messagingSenderId: "__NEXT_PUBLIC_FIREBASE_MESSAGING_SENDER_ID__",
  appId: "__NEXT_PUBLIC_FIREBASE_APP_ID__",
});

const messaging = firebase.messaging();

// Payload contract, docs/SERVER_PLAN.md §7.6 -- all values are strings
// (FCM data maps are string-only): `kind` (`message` | `geofence`),
// `convKey`, `id`, `senderUid`, `senderAlias`, `title`, `body`, `url` (the
// path to open, e.g. `/chat/{alias}`).
messaging.onBackgroundMessage((payload) => {
  const data = payload.data || {};
  const title = data.title || "Pager";
  const body = data.body || "";
  const url = data.url || "/chat";

  // Kept as a real switch (rather than one shared options object) so a kind
  // added later can get its own tag/icon without touching the other case --
  // today `message` and `geofence` differ only in how notifications from the
  // same kind collapse into one (tag), not in title/body/url handling.
  let options;
  switch (data.kind) {
    case "message":
      options = {
        body,
        icon: "/icons/icon-192.png",
        tag: data.convKey ? `pager-thread-${data.convKey}` : undefined,
        data: { url },
      };
      break;
    case "geofence":
      options = {
        body,
        icon: "/icons/icon-192.png",
        tag: data.id ? `pager-geofence-${data.id}` : undefined,
        data: { url },
      };
      break;
    default:
      options = { body, icon: "/icons/icon-192.png", data: { url } };
      break;
  }

  self.registration.showNotification(title, options);
});

self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  const url = (event.notification.data && event.notification.data.url) || "/chat";
  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((clientList) => {
      // Prefer a client already on the right page.
      for (const client of clientList) {
        if ("focus" in client && new URL(client.url).pathname === url) {
          return client.focus();
        }
      }
      // Otherwise reuse any open tab rather than piling up new windows --
      // navigate the first one to the target page, then focus it.
      const [client] = clientList;
      if (client && "focus" in client) {
        if ("navigate" in client) {
          return client.navigate(url).then(() => client.focus());
        }
        return client.focus();
      }
      // No client at all -- nothing to reuse.
      if (self.clients.openWindow) {
        return self.clients.openWindow(url);
      }
      return undefined;
    })
  );
});

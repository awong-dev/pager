// Background push handler -- docs/SERVER_PLAN.md §7.1/§7.6: FCM
// `onBackgroundMessage` -> `showNotification`; `notificationclick` opens the
// thread. A service worker can't import the npm `firebase` package (no
// bundler runs here), so this uses the `-compat` scripts Firebase publishes
// for exactly this case, loaded from the CDN.
//
// Config is duplicated from `web/lib/firebase.ts`'s dev defaults rather than
// templated in, because this file is served byte-for-byte from `public/` --
// Next's `NEXT_PUBLIC_*` inlining only applies to code that actually goes
// through its bundler. A real deployment (out of scope for this build, see
// web/README.md) must fill in its real Firebase project's config here too.
importScripts("https://www.gstatic.com/firebasejs/12.3.0/firebase-app-compat.js");
importScripts("https://www.gstatic.com/firebasejs/12.3.0/firebase-messaging-compat.js");

firebase.initializeApp({
  apiKey: "demo-api-key",
  authDomain: "demo-pager.firebaseapp.com",
  projectId: "demo-pager",
  storageBucket: "demo-pager.appspot.com",
  messagingSenderId: "0",
  appId: "1:0:web:0",
});

const messaging = firebase.messaging();

messaging.onBackgroundMessage((payload) => {
  const data = payload.data || {};
  const title = data.title || `New message from @${data.senderAlias || "pager"}`;
  const options = {
    body: data.body || payload.notification?.body || "",
    icon: "/icons/icon-192.png",
    tag: data.convKey ? `pager-thread-${data.convKey}` : undefined,
    data: { alias: data.senderAlias || null },
  };
  self.registration.showNotification(title, options);
});

self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  const alias = event.notification.data && event.notification.data.alias;
  const url = alias ? `/chat/${encodeURIComponent(alias)}` : "/chat";
  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((clientList) => {
      for (const client of clientList) {
        if ("focus" in client) {
          client.navigate(url);
          return client.focus();
        }
      }
      if (self.clients.openWindow) {
        return self.clients.openWindow(url);
      }
      return undefined;
    })
  );
});

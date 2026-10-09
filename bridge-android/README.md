# bridge-android — the bridge phone app

Design: `docs/BRIDGE_PHONE_DESIGN.md` (decision 13 and the 9 Oct 2026 "Orchestrator decisions"
O1/O2/O4); tasks A1–A7 in `docs/BRIDGE_PHONE_TASKS.md`. A sideloaded Android app that turns a
headless phone into a relay for Google Chat, Google Voice and SIM SMS: it reads the two Google
apps' `MessagingStyle` notifications, is the default SMS app, replies through the notification
`RemoteInput` (tier 1) or, failing that, by typing into the app with an accessibility service
(tier 2), and talks to the relay over `/bridge/pair`, `/bridge/events`, `/bridge/outbox`,
`/bridge/outbox/{id}/ack`, `/bridge/heartbeat`.

Status (9 Oct 2026): builds and unit-tests green on the Mac; **never run on a phone** (no device
was attached). The relay side (B1–B9) had not landed when this was written, so the wire shapes are
taken from the design text, not from a running server.

## Build

```sh
cd bridge-android
export JAVA_HOME=/opt/homebrew/opt/openjdk@17      # AGP 8.7 refuses the default JDK 25
./gradlew assembleDebug testDebugUnitTest
# -> app/build/outputs/apk/debug/app-debug.apk
```

`local.properties` (`sdk.dir=$HOME/Library/Android/sdk`) is git-ignored and must exist. The
wrapper pins Gradle 8.10.2; versions are in `gradle/libs.versions.toml` (AGP 8.7.3, Kotlin
2.0.21, KSP, Room 2.6.1, WorkManager 2.10, security-crypto, OkHttp 4.12, kotlinx-serialization
1.7.3, Firebase BOM 33.7). No Compose; plain Views with ViewBinding. No analytics, no SDKs
beyond AndroidX, OkHttp and (push build only) Firebase Messaging.

### Poll-only vs push build (A7)

`app/build.gradle.kts` applies `com.google.gms.google-services` and adds Firebase Messaging
**only if `app/google-services.json` exists**; `BuildConfig.FCM` follows. Without the file the
app polls `GET /bridge/outbox` every 60 s (30–300 s on the setup screen) and the outbound
latency is ≤ the poll period (O2). The owner step to get the push build:

1. Firebase console → the pager project → Add app → Android, package `app.kidpager.bridge`
   (or `firebase apps:create android app.kidpager.bridge --project <prod project>` then
   `firebase apps:sdkconfig android <appId>`).
2. Save the file as `bridge-android/app/google-services.json` (git-ignored) and rebuild;
   `BuildConfig.FCM` becomes `true`, `src/fcm/java/FcmService.kt` (the real
   `FirebaseMessagingService`) replaces the `src/nofcm` placeholder, and the relay's
   `{kind: "outbox"}` data push wakes the outbox worker at once. The FCM token travels in the
   heartbeat body (`fcmToken`).

## Sideload and set up the phone

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n app.kidpager.bridge/.SetupActivity
```

On the phone, in this order (the setup screen has a button or status row for each):

1. Sign the phone into the bridge member's own Google account; install Google Chat and Google
   Voice, sign both in, notifications **on** for both. **Keep this account's contacts list
   empty** so Voice shows the peer's number, not a name (O4: the sender number is read from the
   notification; a name-only sender is reported with `sender.name` and the relay drops it).
2. Open Pager Bridge → *Grant runtime permissions* (SMS, phone, accounts, notifications).
3. *Open notification access settings* → enable Pager Bridge (A3, tier 1 and inbound Chat/Voice).
4. *Make default SMS app* (A4; only if the phone has a SIM you want to text through).
5. *Exempt from battery optimisation* (keeps the foreground service alive and lets the boot
   receiver / WorkManager restart it).
6. *Open accessibility settings* → enable Pager Bridge (A6, tier 2 and `inspect`). Without it
   outbox items that need tier 2 ack `failed no_accessibility`.
7. Fill *Relay URL*, check the auto-filled *SIM number* (blank = no SIM), enter the *Google Voice
   number* if the account has one (O1: a Voice-only bridge is fine), optionally the account
   emails if the device does not expose them, then *Save fields*.
8. Paste the 8-digit code from Family → Devices → Add bridge phone and tap *Pair*. The token is
   stored in `EncryptedSharedPreferences` and the service starts; the Devices row shows
   `lastSeenAt` after the first heartbeat (5 min in-service timer, 15-min WorkManager backstop).
9. Phone hygiene (decision 13): screen lock **None**, Do Not Disturb off, never leave Chat open
   on a thread, charge limiter / smart-plug duty cycle, `adb tcpip 5555` for maintenance.

A 401 from the relay clears the token and posts a persistent "needs re-pairing" notification;
pair again with a new code (Devices → New code).

## What each permission is for

| permission | used by |
|---|---|
| `BIND_NOTIFICATION_LISTENER_SERVICE` (user-granted) | `ChatNotificationListener`: inbound Chat/Voice text, reply-action cache |
| `BIND_ACCESSIBILITY_SERVICE` (user-granted) | `BridgeAccessibilityService`: tier-2 send, `inspect` |
| `RECEIVE_SMS`, `RECEIVE_MMS`, `RECEIVE_WAP_PUSH`, `READ_SMS`, `SEND_SMS` + the SMS role | `SmsReceiver`, `MmsReceiver`, `SmsSender`; the role is what makes `SMS_DELIVER` fire |
| `READ_PHONE_STATE`, `READ_PHONE_NUMBERS` | `Status.simNumber` (auto-fill; editable) and the SMS subscription id |
| `GET_ACCOUNTS` | `Status.accounts` (Google account emails for the pair request; can be typed instead) |
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_REMOTE_MESSAGING` | `BridgeService` (type `remoteMessaging`, no 6-hour cap on Android 15) |
| `RECEIVE_BOOT_COMPLETED` | `BootReceiver` restarts the service |
| `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` | setup button |
| `POST_NOTIFICATIONS` | the service notification and the re-pair alert |
| `INTERNET` | `RelayClient` |

## Code map (every class header names the decision it implements)

- `Targets.kt` — package names, deep-link patterns, Voice thread URL template, accessibility
  selectors. **UI churn in Chat/Voice is fixed here only.**
- `Log.kt` — the one log wrapper (logcat + 500-line ring buffer → `LogActivity`); numbers redacted.
- `RelayClient.kt` — the five `/bridge` calls; bearer token; backoff on 5xx/IO; 401 handling.
- `Events.kt` — `BridgeEvent` wire model and the decision-5 bounds (`Bounds`).
- `NotificationMapper.kt` (pure) + `ChatNotificationListener.kt` — snapshot → events; O4 number
  resolution; dedup keys. `Db.kt` — Room (`seen`, 7-day TTL; `pending_events` spool).
- `EventQueue.kt` — 500 ms debounce, batches ≤ 50, retry with backoff until 2xx.
- `ReplyCache.kt` — conversation → reply action (+ Voice phone → conversation); tier 1.
- `SmsReceiver.kt`, `MmsReceiver.kt` (+ `MmsFileProvider`), `MmsPdu.kt` (pure parser),
  `SmsSender.kt` (+ `SmsResultReceiver`), `HeadlessSmsSendService.kt`, `ComposeActivity.kt`.
- `BridgeAccessibilityService.kt` — tier 2 `sendViaUi`, `inspect`, 20-s cap, always HOME.
- `OutboxWorker.kt` + `AckTracker.kt` (pure) + `Dispatcher.kt` — O2 polling, tier dispatch, acks.
- `BridgeService.kt`, `BootReceiver.kt`, `HeartbeatWorker.kt`, `Notifications.kt`, `Status.kt`,
  `Prefs.kt`, `SetupActivity.kt`, `LogActivity.kt`, `FcmTokens.kt`, `src/{fcm,nofcm}/.../FcmService.kt`.

Unit tests (`app/src/test`, JUnit 4, 30 tests): mapper (ids, self-filter, dedup, Voice numbers,
bounds), phone normalisation and link helpers, `AckTracker` state machine (bounded ack retries,
404-forget, re-issue after purge), MMS PDU parsing, the MMS and SMS pure event steps, SMS
event shape and JSON.

## Fields to verify on the bench (design vs. what the apps really emit)

- **Conversation id.** `shortcutId` when set, else `pkg|id` (the `sbn.key` minus user, tag and
  uid — the design says "account/tag stripped"; if Chat distinguishes threads only by tag this
  collapses them and the fallback must include the tag). An `inspect` event uses
  `Targets.conversationIdFromLink` (`room/<id>`, `dm/<id>`, Voice `itemId`), which may **not**
  equal the notification's `shortcutId`; if the two differ the subscribe-by-link row will not
  match later messages, and the relay or `Targets` needs one normalisation rule once the real
  ids are known.
- **Voice sender number.** Tried in order: `shortcutId` / `EXTRA_SUB_TEXT` (O4), the sender line,
  the title. Which of these Voice actually fills is unknown until a text is observed on hardware.
- **Own-message filter.** A `MessagingStyle` message with `person == null` or whose name equals
  `style.user.name` is treated as the bridge account's own line and skipped.
- **Heartbeat response.** Parsed as `{pending: n}`; an empty body (204) counts as 0.
- **Outbox item shape.** `OutboxItem {id, kind, source, to{phone, conversationId, link}, text,
  link, msgId, bid, replyHint, sim}`; `inspect` reads `link` or `to.link`.
- **`ts`** is Unix epoch **seconds** (as everywhere else in the relay); the design does not say.
- **Dual SIM.** `sim` on the outbox item is taken as a subscription id; the design never defines
  how the relay would know it.
- **MMS.** The M-Retrieve.conf is downloaded and parsed; no M-NotifyResp.ind / M-Acknowledge.ind
  is sent back, so a carrier may redeliver the notification a few times (dedup by event id covers
  it). Group MMS is reported with `isGroup=false` (the relay drops groups anyway).
- **SMS provider.** Inbound texts are not written to the system SMS store (headless phone).

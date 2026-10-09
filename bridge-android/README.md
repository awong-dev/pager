# bridge-android — the bridge phone app

Design: `docs/BRIDGE_PHONE_DESIGN.md` (decision 13, the 9 Oct 2026 "Orchestrator decisions"
O1/O2/O4 and WhatsApp WA1–WA7); tasks A1–A9 in `docs/BRIDGE_PHONE_TASKS.md`. A sideloaded
Android app that turns a headless phone into a relay for Google Chat, Google Voice, WhatsApp and
SIM SMS: it reads the three chat apps' `MessagingStyle` notifications, is the default SMS app,
replies through the notification
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

### WhatsApp (A9, WA1–WA7)

No number field: the relay never needs the WhatsApp account's number. The setup screen's
*WhatsApp* row shows installed yes/no and whether it is bridged (installed **and** notification
access on); that same bool goes out as `caps.whatsapp` at pairing and `status.whatsapp` on every
heartbeat, so installing WhatsApp after pairing is picked up on the next heartbeat.

1. Install WhatsApp (`com.whatsapp`; WhatsApp Business `com.whatsapp.w4b` is read the same way)
   and register it with the **SIM number** of this phone.
2. WhatsApp notifications **on**, with message **previews on** (Settings → Notifications; also the
   Android channel must show content on the shade — the listener reads the `MessagingStyle` lines,
   not the ticker). "High priority notifications" on is fine.
3. **No contacts** on the phone: an unsaved DM sender shows as the number, and the DM JID
   (`<digits>@s.whatsapp.net`) carries the number anyway. Group members without a contact show as
   `~ Name` (the tilde is stripped).
4. **Archive or mute nothing** you want bridged: muted and archived chats post no notifications,
   so they never reach the relay.
5. Do not open WhatsApp on the phone while a reply is in flight (tier 2 types into whatever chat
   is on screen).

Tier 2 notes:
- **Ban risk.** Tier 2 drives the WhatsApp UI with the accessibility service. WhatsApp's terms
  forbid automation and its anti-abuse systems have banned accounts for far less; keep this
  account disposable (a second SIM, not the family's main number). Tier 1 (the notification
  reply action) is ordinary and is what runs almost every time.
- **DM tier 2** opens `https://wa.me/<digits>` in WhatsApp, which lands in the chat composer for
  a known number, then the usual composer/Send/verify steps.
- **Group tier 2 is best-effort.** There is no deep link to a WhatsApp group
  (`chat.whatsapp.com/...` is a *join* link), so the service launches WhatsApp, taps the chat-list
  search, types the group title and clicks the first row whose name equals it. Two groups with the
  same title, a renamed group, or a title WhatsApp elides will fail with `no_match`/`ui_changed`;
  the relay keeps the item for a retry once a fresh notification restores the tier-1 action.

A 401 from the relay clears the token and posts a persistent "needs re-pairing" notification;
pair again with a new code (Devices → New code).

## What each permission is for

| permission | used by |
|---|---|
| `BIND_NOTIFICATION_LISTENER_SERVICE` (user-granted) | `ChatNotificationListener`: inbound Chat/Voice/WhatsApp text, reply-action cache |
| `BIND_ACCESSIBILITY_SERVICE` (user-granted) | `BridgeAccessibilityService`: tier-2 send, `inspect` |
| `RECEIVE_SMS`, `RECEIVE_MMS`, `RECEIVE_WAP_PUSH`, `READ_SMS`, `SEND_SMS` + the SMS role | `SmsReceiver`, `MmsReceiver`, `SmsSender`; the role is what makes `SMS_DELIVER` fire |
| `READ_PHONE_STATE`, `READ_PHONE_NUMBERS` | `Status.simNumber` (auto-fill; editable) and the SMS subscription id |
| `GET_ACCOUNTS` | `Status.accounts` (Google account emails for the pair request; can be typed instead) |
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_REMOTE_MESSAGING` | `BridgeService` (type `remoteMessaging`, no 6-hour cap on Android 15) |
| `RECEIVE_BOOT_COMPLETED` | `BootReceiver` restarts the service |
| `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` | setup button |
| `POST_NOTIFICATIONS` | the service notification and the re-pair alert |
| `INTERNET` | `RelayClient` |
| `<queries>` for the Chat, Voice and WhatsApp packages | `Status.whatsappPackage` and `getLaunchIntentForPackage` (package visibility, API 30+) |

## Code map (every class header names the decision it implements)

- `Targets.kt` — package names, deep-link patterns, Voice thread URL template, WhatsApp link /
  JID helpers and media-placeholder table, accessibility selectors. **UI churn in
  Chat/Voice/WhatsApp is fixed here only.**
- `Log.kt` — the one log wrapper (logcat + 500-line ring buffer → `LogActivity`); numbers redacted.
- `RelayClient.kt` — the five `/bridge` calls; bearer token; backoff on 5xx/IO; 401 handling.
- `Events.kt` — `BridgeEvent` wire model and the decision-5 bounds (`Bounds`).
- `NotificationMapper.kt` (pure) + `ChatNotificationListener.kt` — snapshot → events; O4 number
  resolution; WhatsApp JID phone, `~ ` stripping, "You" skip, media placeholders → `attachments`;
  dedup keys; group-summary notifications skipped. `Db.kt` — Room (`seen`, 7-day TTL; `pending_events` spool).
- `EventQueue.kt` — 500 ms debounce, batches ≤ 50, retry with backoff until 2xx.
- `ReplyCache.kt` — conversation → reply action (+ per-source phone → conversation for Voice and
  WhatsApp DMs); tier 1.
- `SmsReceiver.kt`, `MmsReceiver.kt` (+ `MmsFileProvider`), `MmsPdu.kt` (pure parser),
  `SmsSender.kt` (+ `SmsResultReceiver`), `HeadlessSmsSendService.kt`, `ComposeActivity.kt`.
- `BridgeAccessibilityService.kt` — tier 2 `sendViaUi` (link), `sendViaSearch` (WhatsApp group
  by title, 30-s cap), `inspect`, 20-s cap, always HOME.
- `OutboxWorker.kt` + `AckTracker.kt` (pure) + `Dispatcher.kt` — O2 polling, tier dispatch, acks.
- `BridgeService.kt`, `BootReceiver.kt`, `HeartbeatWorker.kt`, `Notifications.kt`, `Status.kt`,
  `Prefs.kt`, `SetupActivity.kt`, `LogActivity.kt`, `FcmTokens.kt`, `src/{fcm,nofcm}/.../FcmService.kt`.

Unit tests (`app/src/test`, JUnit 4, 41 tests): mapper (ids, self-filter, dedup, Voice numbers,
WhatsApp DM/LID/group/media mapping, bounds), phone normalisation, JID and link helpers
(`TargetsTest`), `AckTracker` state machine (bounded ack retries, 404-forget, re-issue after
purge), MMS PDU parsing, the MMS and SMS pure event steps, SMS event shape and JSON.

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

### WhatsApp: verify on the bench (nothing below has been seen on a device)

Every WhatsApp selector lives in `Targets.kt`; change it there when a bench run disagrees.

- **`shortcutId` is the JID.** DM `<digits>@s.whatsapp.net`, group `<id>@g.us`, privacy-mode
  `<digits>@lid` (no number; the relay drops the event unless the sender line is a number). If
  WhatsApp stops setting `shortcutId`, the conversation id falls back to `pkg|id` and DMs lose
  their phone unless the sender line / title is the number.
- **Group-summary skip.** `FLAG_GROUP_SUMMARY` ("N messages from M chats") is dropped before
  mapping; per-chat notifications are expected to be the `MessagingStyle` ones.
- **`MessagingStyle.user.name`** for WhatsApp is assumed to be "You"; the sender "You" skip
  covers the group lines either way.
- **Media placeholders** (`Targets.WA_MEDIA`): `📷 Photo`, `🎥 Video`, `🎤 Voice message`,
  `🎵 Audio`, `📄 <name>`, `📍 Location`, `👤 Contact`, `GIF`, `Sticker` — English-locale strings;
  the exact emoji and labels, and whether a caption replaces the label (`📷 <caption>`), are
  from memory of the shade. A document's file name is kept as the event text.
- **Search affordance.** Resource id contains `menuitem_search` or `search`
  (`Targets.WA_SEARCH_ID_HINTS`), or contentDescription `Search` (`WA_SEARCH_DESC`); the search
  field is then the first editable node.
- **Chat-list row name.** Resource id contains `conversations_row_contact_name` /
  `conversation_contact_name` / `contact_name` (`WA_ROW_NAME_ID_HINTS`); falls back to any
  non-editable node whose text equals the title (trim, case-insensitive). Row match waits ≤ 5 s
  (`SEARCH_TIMEOUT_MS`).
- **Composer.** Resource id contains `entry` (`WA_COMPOSER_ID_HINTS`), else any editable node
  whose text is not the title we typed into search.
- **Send button.** contentDescription `Send` (shared `SEND_DESCRIPTION`).
- **Verify.** The sent text must appear in a non-editable node within 5 s; WhatsApp's bubble text
  is expected to be a plain `TextView`.
- **`wa.me` handling.** `https://wa.me/<digits>` opened with `setPackage(com.whatsapp)` is
  expected to land in the chat composer; if WhatsApp shows an interstitial ("Message +1 …?")
  the composer wait (15 s) will hit `ui_changed` and a click on that interstitial needs adding.
- **WhatsApp Business only.** `Status.whatsappPackage` prefers `com.whatsapp`, then
  `com.whatsapp.w4b`; `wa.me` links fall back to Business when the consumer app is absent.

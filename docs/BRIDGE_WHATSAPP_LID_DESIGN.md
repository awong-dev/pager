# WhatsApp DMs keyed by conversation id (LID), 10 Oct 2026

*(Live finding, 10 Oct 2026: the first real WhatsApp DM through the bridge phone arrived as conversation
`<digits>@lid`, sender `Albert`, no number; the relay answered `dropped_bad_from`. WhatsApp now names many
chats by a LID (linked identifier), so the listener sees only the LID and the display name. This supersedes
WA2 and WA5's WhatsApp half in `docs/BRIDGE_PHONE_DESIGN.md`. Tasks: B13 and A10 in `docs/BRIDGE_PHONE_TASKS.md`.)*

## Decisions

| # | Question | Decision | Why |
|---|---|---|---|
| L1 | Identity of a WhatsApp DM | The bridge conversation row `(bridgeId, conversation.id)` for every WhatsApp DM, LID (`<digits>@lid`) and phone JID (`<digits>@s.whatsapp.net`) alike. A DM takes `_handle_chat_event`, like a Google Chat DM. | The LID is stable per peer for one WhatsApp account, and the conversation id is the only key every WhatsApp DM has. With one path, the parent always gets the same alert for WhatsApp. |
| L2 | Phone JID present: keep the WA2 number path? | No. The number path is used for SMS and Voice only. A number on a WhatsApp event is a hint: Android puts it in `conversation.link` (`wa.me/<digits>`) for tier 2. The relay never uses it as identity. | WhatsApp is moving chats to LIDs, so the phone JID will become rare. Keeping both paths gives two alert kinds (`sms_unknown` vs `chat_unknown`), and which one appears would depend on WhatsApp internals. Per the no-A/B rule, the WA2 code is deleted, not toggled. |
| L3 | Cost of L2 | A WhatsApp peer who is also an SMS contact becomes a second external (`t:"chat"`). Its pager name must differ (the existing 409 on name collision), e.g. "Grandma WA". WA5's "last inbound channel wins" no longer covers WhatsApp. | Accepted. Merging a LID with a phone contact cannot be done safely, because the listener never sees both for the same peer. |
| L4 | Title of a DM row | Relay: `title = conversation.title or sender.name` when `isGroup` is false (all sources). | WhatsApp 1:1 MessagingStyle often has no `conversationTitle`. Without a title the card says "Untitled conversation" and tier 2 has nothing to search for. Doing this on the relay means the installed APK keeps working. |
| L5 | What is still dropped | `dropped_bad_from` stays only on the SMS/Voice text path (no parseable number). New `dropped_bad_conv` for a `whatsapp` event whose `conversation.id` does not match `^\d+@(lid\|s\.whatsapp\.net)$` or `^[\d-]+@g\.us$`. An empty `sender.name` is labelled "someone" (as today), not dropped. | The listener falls back to `pkg\|notificationId` when there is no shortcut id. That id changes on every notification, so each message would create a new held conversation and alert. The schema already rejects an empty id. |
| L6 | Outbound | No change. The bridge chat backend sends `to.conversationId` and `to.title`, plus `to.link` when the row has one. Android: tier 1 is the ReplyCache by conversation id; tier 2 opens `to.link` if set, otherwise searches by title (`Dispatcher.whatsapp`, already the path for a non-group with no phone). | Tier 1 already keys on the conversation id. For a LID, title search is the only tier 2 available. |
| L7 | Alert and UI | `chat_unknown` already carries `source`. `AlertCard` already shows "Direct message · WhatsApp", and `/family/chat` already shows a source chip (`sourceLabel`). Only change: `pushBody` for a non-group row reads `"<Source>: <title> → @alias: <text>"` instead of `"<title> (<sender>) → ..."`, where Source is `sourceLabel` (WhatsApp, Google Chat, Google Voice). | Today a DM push reads "Albert (Albert) → @kid" and doesn't name the app. |
| L8 | Android | Delete the LID warning and `lidLogged`. For a phone-JID DM, set `conversation.link = Targets.waLink(phone)`. Keep `sender.phone` as is (the relay ignores it for `whatsapp`). Drop the WhatsApp half of `ReplyCache.rememberPhone` / `conversationForPhone`; Voice keeps its half. | The warning is now wrong. The link gives phone-JID DMs a deep-link tier 2, which is better than title search. A WhatsApp send no longer carries `to.phone`, so the phone map is dead code. |
| L9 | Pager / PROTOCOL.md | No change. A subscribed WhatsApp DM is a chat external, so it projects as `t:"chat"`, which already exists in §3.1. Groups stay `t:"grp"`. Before this change a WhatsApp DM was `t:"sms"` or `cfg.sms`. | The pager sees no new field and no new value. |
| L10 | Migration | None. No WhatsApp DM has ever been bridged (the first one was dropped), so no `via: whatsapp` or `voiceConv` JID rows exist in production. Production holds test data. | See the overnight-runs rule: no migrations while production is test data. |

## Data flow (DM, either JID shape)
1. Listener → `POST /bridge/events {source: whatsapp, conversation: {id: "<n>@lid", isGroup: false}, sender: {name}}`.
2. `process_event`: every `whatsapp` event goes to `_handle_chat_event`, after the L5 shape check.
3. Three separate transactions, all existing code. (a) `upsert_seen` writes `bridgeConversations/{rid}` with source `whatsapp` and the L4 title. (b) `held_chat_store.create(br_<bridge>_<eventId>)` is the dedup on the wire id; it is a create-if-absent, so a retried event returns `duplicate`. (c) `chat_held_upsert` keeps one open `chat_unknown` per `(bridge, conv)`. If the process crashes between (b) and (c), the next text repairs it, because `heldCount` is counted from the rows.
4. Parent, on /family/chat: Subscribe → `get_or_create_chat` (a chat external, `phone: null`), the allow edge `owner→ext`, a book bump (`t:"chat"`), and the held backlog is released.
5. Later messages: `deliver_subscribed`. A pager reply goes to the `bridge` backend, then an outbox `send {source: whatsapp, to: {conversationId, title, link?}}`, then Android tier 1 or tier 2.

Invariants are unchanged. The relay is the only writer. The allow-list is enforced by the subscribe edge and by `firestore.rules`, which are untouched. Delivery state stays monotonic through `apply_outbox_state`. `/bridge/events` stays bearer-authenticated.

## Failure modes
| Failure | Seen as | Recovery |
|---|---|---|
| A peer's id changes from phone JID to LID (WhatsApp migration) | A second held conversation with the same title, and a second alert | The parent ignores it or subscribes it with a new name, then unsubscribes the old one |
| A DM title search matches the wrong row or no row (LID peer not in contacts) | tier 2 `no_match` / `ui_changed` | Tier 1 works while the reply action exists. Title search is best effort (verify on bench) |
| Notification without a shortcut id | `dropped_bad_conv`, one INFO line per event | None. Bench check that WhatsApp DMs always carry a shortcut id |
| Two DM peers with the same display name | Two rows with the same title. The pager names collide (409) at subscribe | The parent picks distinct names |

## What to measure
- `bridge in src=whatsapp outcome=` per day. Expect `held`/`delivered`, `dropped_bad_conv` ≈ 0, and `dropped_bad_from` never for `whatsapp`.
- Share of WhatsApp DM conv ids that are `@lid` vs `@s.whatsapp.net` (log `kind=lid|jid|grp` on the INFO line).
- `bridge out src=whatsapp tier=`: the tier-2 share for DMs, and `no_match` count.
- Per bridge, WhatsApp rows with duplicate titles (a sign of L3 / the JID→LID switch).

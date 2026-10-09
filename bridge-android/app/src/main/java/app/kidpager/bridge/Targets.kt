package app.kidpager.bridge

import java.net.URLEncoder

/**
 * Decision 13: every app-specific string that can rot with an app update lives here --
 * notification package names, deep-link patterns and the accessibility selectors tier 2 uses.
 * UI churn in Chat, Voice or WhatsApp is a one-file fix. WhatsApp (WA1-WA7, 9 Oct 2026) is the
 * third source; its selectors are "verify on bench" (bridge-android/README.md).
 */
object Targets {
    const val GCHAT_PKG = "com.google.android.apps.dynamite"
    const val GVOICE_PKG = "com.google.android.apps.googlevoice"
    const val WHATSAPP_PKG = "com.whatsapp"
    const val WHATSAPP_BUSINESS_PKG = "com.whatsapp.w4b"

    /** Packages the notification listener reads (decision 13 listener bullet, WA6). */
    val LISTEN_PACKAGES = setOf(GCHAT_PKG, GVOICE_PKG, WHATSAPP_PKG, WHATSAPP_BUSINESS_PKG)

    const val SOURCE_SMS = "sms"
    const val SOURCE_GCHAT = "gchat"
    const val SOURCE_GVOICE = "gvoice"
    const val SOURCE_WHATSAPP = "whatsapp"

    fun sourceFor(pkg: String): String? = when (pkg) {
        GCHAT_PKG -> SOURCE_GCHAT
        GVOICE_PKG -> SOURCE_GVOICE
        WHATSAPP_PKG, WHATSAPP_BUSINESS_PKG -> SOURCE_WHATSAPP
        else -> null
    }

    // ---- deep links (decision 10: chat.google.com, mail.google.com/chat, voice.google.com; WA4: wa.me) ----
    val CHAT_LINK = Regex("^https://(chat\\.google\\.com|mail\\.google\\.com/chat)(/|$)")
    val VOICE_LINK = Regex("^https://voice\\.google\\.com(/|$)")
    /** `https://wa.me/<digits>`, `https://api.whatsapp.com/send?phone=<digits>`, `whatsapp://send?phone=<digits>`. */
    val WA_LINK = Regex("^(https://wa\\.me/|https://api\\.whatsapp\\.com/send\\?|whatsapp://send\\?)", RegexOption.IGNORE_CASE)

    /** Which app a stored link opens in; null when the link is not one of ours. */
    fun packageForLink(link: String): String? = when {
        CHAT_LINK.containsMatchIn(link) -> GCHAT_PKG
        VOICE_LINK.containsMatchIn(link) -> GVOICE_PKG
        WA_LINK.containsMatchIn(link) -> WHATSAPP_PKG
        else -> null
    }

    /** Google Voice thread deep link for a peer number (used when a `gvoice` send has only `to.phone`). */
    fun voiceThreadLink(e164: String): String =
        "https://voice.google.com/u/0/messages?itemId=t." + URLEncoder.encode(e164, "UTF-8")

    /** WA4: WhatsApp chat link for a peer number; WhatsApp opens the composer for a known number. */
    fun waLink(e164: String): String = "https://wa.me/" + e164.filter { it.isDigit() }

    // ---- WhatsApp JIDs (WA2/WA3): `<digits>@s.whatsapp.net` DM, `<id>@g.us` group, `<digits>@lid` no number ----
    val WA_DM_JID = Regex("^(\\d{6,15})@s\\.whatsapp\\.net$")
    val WA_GROUP_JID = Regex("^[\\d-]+@g\\.us$")
    val WA_LID_JID = Regex("^\\d+@lid$")

    /** `+<digits>` from a DM JID, null for anything else (groups, LIDs, opaque ids). */
    fun phoneFromWaJid(jid: String?): String? = jid?.let { WA_DM_JID.find(it.trim())?.groupValues?.get(1) }?.let { "+$it" }

    /** The DM JID for an E.164 number (the conversation id WhatsApp puts on its notifications). */
    fun waDmJid(e164: String): String = e164.filter { it.isDigit() } + "@s.whatsapp.net"

    /**
     * Conversation id derived from a pasted link, used for `inspect` events (decision 10).
     * Chat: `.../room/<id>...` or `.../dm/<id>...` -> `<kind>/<id>`; Voice: `itemId=<id>`;
     * WhatsApp: the DM JID `<digits>@s.whatsapp.net` from a `wa.me` / `send?phone=` link.
     * Whether this equals the `shortcutId` the apps put on their notifications is a bench check
     * (bridge-android/README.md "Fields to verify").
     */
    fun conversationIdFromLink(link: String): String? {
        if (WA_LINK.containsMatchIn(link)) {
            val digits = Regex("^https://wa\\.me/\\+?(\\d{6,15})", RegexOption.IGNORE_CASE).find(link)?.groupValues?.get(1)
                ?: Regex("[?&]phone=\\+?(?:%2B)?(\\d{6,15})", RegexOption.IGNORE_CASE).find(link)?.groupValues?.get(1)
                ?: return null
            return waDmJid(digits)
        }
        Regex("/(room|dm|space)/([A-Za-z0-9_-]+)").find(link)?.let { return "${it.groupValues[1]}/${it.groupValues[2]}" }
        Regex("[?&]itemId=([^&#]+)").find(link)?.let { return java.net.URLDecoder.decode(it.groupValues[1], "UTF-8") }
        return null
    }

    // ---- accessibility selectors (decision 13 BridgeAccessibilityService bullet) ----
    /** Composer: a node that `isEditable` or whose className contains this. */
    const val COMPOSER_CLASS_HINT = "EditText"
    /** Send button: contentDescription (or text) matching this, clickable itself or via a parent. */
    val SEND_DESCRIPTION = Regex("^send( message)?$", RegexOption.IGNORE_CASE)
    /** Inspect: resource-id fragments that mark the thread title. */
    val TITLE_ID_HINTS = listOf("toolbar_title", "title", "conversation_name", "room_name")
    /** Inspect: resource-id fragments that mark a message's sender name. */
    val SENDER_ID_HINTS = listOf("sender", "author", "user_name", "display_name", "contact_name")

    // ---- WhatsApp selectors (WA4 group tier 2; every one is "verify on bench") ----
    /** Toolbar search on the chat list: resource-id fragments. */
    val WA_SEARCH_ID_HINTS = listOf("menuitem_search", "search")
    /** Toolbar search on the chat list: contentDescription. */
    val WA_SEARCH_DESC = Regex("^search$", RegexOption.IGNORE_CASE)
    /** Chat-list row name: resource-id fragments. */
    val WA_ROW_NAME_ID_HINTS = listOf("conversations_row_contact_name", "conversation_contact_name", "contact_name")
    /** Message composer inside a chat: resource-id fragments (the generic `isEditable` probe is the fallback). */
    val WA_COMPOSER_ID_HINTS = listOf("entry")

    /**
     * WA6 media placeholders as WhatsApp renders them in notification text (English locale):
     * leading token -> attachment kind. `📷 Photo` alone is a placeholder; `📷 <caption>` keeps the
     * caption as text. Checked by [NotificationMapper.waMedia].
     */
    val WA_MEDIA = listOf(
        WaMedia("📷", "Photo", "image"),
        WaMedia("🎥", "Video", "video"),
        WaMedia("🎤", "Voice message", "audio"),
        WaMedia("🎵", "Audio", "audio"),
        WaMedia("📄", "Document", "file"),
        WaMedia("📍", "Location", "file"),
        WaMedia("👤", "Contact", "file"),
        WaMedia("GIF", null, "image"),
        WaMedia("Sticker", null, "image"),
    )

    /** `prefix` opens the line; `label` is the bare-placeholder word (null: the prefix alone is the whole line). */
    data class WaMedia(val prefix: String, val label: String?, val kind: String)

    const val OPEN_TIMEOUT_MS = 15_000L
    const val VERIFY_TIMEOUT_MS = 5_000L
    const val TIER2_TIMEOUT_MS = 20_000L
    /** WA4 group tier 2: wait for a chat-list row matching the title after typing it into search. */
    const val SEARCH_TIMEOUT_MS = 5_000L
}

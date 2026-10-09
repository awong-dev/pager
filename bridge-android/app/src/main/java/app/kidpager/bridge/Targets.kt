package app.kidpager.bridge

import java.net.URLEncoder

/**
 * Decision 13: every app-specific string that can rot with a Google app update lives here --
 * notification package names, deep-link patterns and the accessibility selectors tier 2 uses.
 * UI churn in Chat or Voice is a one-file fix.
 */
object Targets {
    const val GCHAT_PKG = "com.google.android.apps.dynamite"
    const val GVOICE_PKG = "com.google.android.apps.googlevoice"

    /** Packages the notification listener reads (decision 13 listener bullet). */
    val LISTEN_PACKAGES = setOf(GCHAT_PKG, GVOICE_PKG)

    const val SOURCE_SMS = "sms"
    const val SOURCE_GCHAT = "gchat"
    const val SOURCE_GVOICE = "gvoice"

    fun sourceFor(pkg: String): String? = when (pkg) {
        GCHAT_PKG -> SOURCE_GCHAT
        GVOICE_PKG -> SOURCE_GVOICE
        else -> null
    }

    // ---- deep links (decision 10: chat.google.com, mail.google.com/chat, voice.google.com) ----
    val CHAT_LINK = Regex("^https://(chat\\.google\\.com|mail\\.google\\.com/chat)(/|$)")
    val VOICE_LINK = Regex("^https://voice\\.google\\.com(/|$)")

    /** Which app a stored link opens in; null when the link is not one of ours. */
    fun packageForLink(link: String): String? = when {
        CHAT_LINK.containsMatchIn(link) -> GCHAT_PKG
        VOICE_LINK.containsMatchIn(link) -> GVOICE_PKG
        else -> null
    }

    /** Google Voice thread deep link for a peer number (used when a `gvoice` send has only `to.phone`). */
    fun voiceThreadLink(e164: String): String =
        "https://voice.google.com/u/0/messages?itemId=t." + URLEncoder.encode(e164, "UTF-8")

    /**
     * Conversation id derived from a pasted link, used for `inspect` events (decision 10).
     * Chat: `.../room/<id>...` or `.../dm/<id>...` -> `<kind>/<id>`; Voice: `itemId=<id>`.
     * Whether this equals the `shortcutId` the apps put on their notifications is a bench check
     * (bridge-android/README.md "Fields to verify").
     */
    fun conversationIdFromLink(link: String): String? {
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

    const val OPEN_TIMEOUT_MS = 15_000L
    const val VERIFY_TIMEOUT_MS = 5_000L
    const val TIER2_TIMEOUT_MS = 20_000L
}

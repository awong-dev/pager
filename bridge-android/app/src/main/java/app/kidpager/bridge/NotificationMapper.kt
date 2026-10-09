package app.kidpager.bridge

/**
 * Decision 13 (listener bullet) and decision 5: the pure mapping from a MessagingStyle
 * notification (already flattened into [NotificationSnapshot] by the listener) to
 * `BridgeEvent`s. Unit-tested in app/src/test; the listener only does the Android calls.
 */
data class SnapshotMessage(val senderName: String?, val text: String?, val timestamp: Long, val isSelf: Boolean)

data class NotificationSnapshot(
    val pkg: String,
    /** `StatusBarNotification.key` (`user|pkg|id|tag|uid`). */
    val key: String,
    val id: Int,
    val shortcutId: String?,
    /** `contentIntent` data / `EXTRA_*` URI when the app exposes one (Voice: `tel:` or thread id). */
    val dataUri: String? = null,
    val conversationTitle: String?,
    val isGroup: Boolean,
    /** `MessagingStyle.user.name`: the signed-in person; their own lines are not inbound. */
    val selfName: String?,
    val messages: List<SnapshotMessage>,
)

data class Mapped(val events: List<BridgeEvent>, val dedupKeys: List<String>, val voicePeerPhone: String?)

object NotificationMapper {
    /**
     * `shortcutId` when set, else the key with the account/tag stripped (`pkg|id`), per
     * decision 13.
     */
    fun conversationId(s: NotificationSnapshot): String =
        s.shortcutId?.takeIf { it.isNotBlank() } ?: "${s.pkg}|${s.id}"

    /**
     * Maps one notification to events for every message not yet seen. `seen(key)` answers the
     * Room dedup table; the returned `dedupKeys` are inserted after the events are spooled.
     */
    fun map(s: NotificationSnapshot, seen: (String) -> Boolean, nowSec: Long = System.currentTimeMillis() / 1000): Mapped {
        val source = Targets.sourceFor(s.pkg) ?: return Mapped(emptyList(), emptyList(), null)
        val convId = conversationId(s)
        // O4: the peer's number from the shortcut id / data URI when present, before the title.
        val voicePhoneFromIds = if (source == Targets.SOURCE_GVOICE) PhoneNumbers.extractE164(s.shortcutId) ?: PhoneNumbers.extractE164(s.dataUri) else null
        val voicePhoneFromTitle = if (source == Targets.SOURCE_GVOICE) PhoneNumbers.normalize(s.conversationTitle) else null
        val events = ArrayList<BridgeEvent>()
        val keys = ArrayList<String>()
        var peerPhone: String? = voicePhoneFromIds ?: voicePhoneFromTitle
        for (m in s.messages) {
            val text = m.text?.toString()?.trim().orEmpty()
            if (text.isEmpty()) continue
            if (m.isSelf) continue
            val name = m.senderName?.trim().orEmpty()
            if (name.isNotEmpty() && s.selfName != null && name == s.selfName.trim()) continue
            val key = Dedup.key(convId, m.timestamp, text)
            if (seen(key)) continue
            var phone: String? = null
            if (source == Targets.SOURCE_GVOICE) {
                // Voice puts the peer's number in the sender or title when there is no contact name.
                // O4: ids, then the sender line, then the title; none -> sender.name only (relay drops it).
                phone = voicePhoneFromIds ?: PhoneNumbers.normalize(name) ?: voicePhoneFromTitle
                if (phone != null) peerPhone = phone
            }
            val tsSec = if (m.timestamp > 0) m.timestamp / 1000 else nowSec
            events += BridgeEvent(
                id = eventId(convId, m.timestamp, text),
                source = source,
                conversation = Conversation(id = Bounds.convId(convId), title = s.conversationTitle?.let { Bounds.cp(it, Bounds.NAME_CP) }, isGroup = s.isGroup),
                sender = Sender(name = Bounds.cp(name.ifEmpty { s.conversationTitle ?: "?" }, Bounds.NAME_CP), phone = phone),
                text = Bounds.cp(text, Bounds.TEXT_CP),
                ts = tsSec,
            )
            keys += key
        }
        return Mapped(events, keys, peerPhone)
    }

    /** Event id, stable for the same message so a relay retry is `duplicate` (decision 5). */
    fun eventId(convId: String, timestamp: Long, text: String): String {
        val digest = java.security.MessageDigest.getInstance("SHA-256")
            .digest("$convId|$timestamp|$text".toByteArray())
        return "n_" + digest.take(12).joinToString("") { "%02x".format(it) }
    }
}

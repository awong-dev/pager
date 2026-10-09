package app.kidpager.bridge

/**
 * Decision 13 (listener bullet) and decision 5: the pure mapping from a MessagingStyle
 * notification (already flattened into [NotificationSnapshot] by the listener) to
 * `BridgeEvent`s. Unit-tested in app/src/test; the listener only does the Android calls.
 * WhatsApp (WA2/WA3/WA6, 9 Oct 2026): DM phone from the JID, group sender `~ ` stripping,
 * "You" lines skipped, media placeholders mapped to `attachments`.
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

/**
 * `peerPhone`: the DM peer's number when the source is phone-keyed (Voice, WhatsApp), so the
 * listener can map phone -> conversation id for tier-1 replies that arrive with only `to.phone`.
 */
data class Mapped(val events: List<BridgeEvent>, val dedupKeys: List<String>, val peerPhone: String?)

object NotificationMapper {
    /** WA6: WhatsApp posts the bridge account's own group lines as sender "You". */
    private const val WA_SELF = "You"

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
        val phoneKeyed = source == Targets.SOURCE_GVOICE || (source == Targets.SOURCE_WHATSAPP && !s.isGroup)
        // O4 / WA2: the peer's number from the shortcut id / data URI when present, before the title.
        val phoneFromIds = when {
            source == Targets.SOURCE_GVOICE -> PhoneNumbers.extractE164(s.shortcutId) ?: PhoneNumbers.extractE164(s.dataUri)
            // WA2: `<digits>@s.whatsapp.net` -> +digits; `<digits>@lid` carries no number (the relay drops it).
            phoneKeyed -> Targets.phoneFromWaJid(s.shortcutId) ?: PhoneNumbers.extractE164(s.dataUri)
            else -> null
        }
        val phoneFromTitle = if (phoneKeyed) PhoneNumbers.normalize(s.conversationTitle) else null
        val events = ArrayList<BridgeEvent>()
        val keys = ArrayList<String>()
        var peerPhone: String? = phoneFromIds ?: phoneFromTitle
        for (m in s.messages) {
            val raw = m.text?.toString()?.trim().orEmpty()
            if (raw.isEmpty()) continue
            if (m.isSelf) continue
            var name = m.senderName?.trim().orEmpty()
            if (name.isNotEmpty() && s.selfName != null && name == s.selfName.trim()) continue
            var text = raw
            var attachments: List<Attachment>? = null
            if (source == Targets.SOURCE_WHATSAPP) {
                // WA3: unsaved members show as `~ Name`; WA6: own lines are "You".
                name = stripWaTilde(name)
                if (name == WA_SELF) continue
                waMedia(raw)?.let { (kind, caption) -> attachments = listOf(Attachment(kind)); text = caption }
            }
            val key = Dedup.key(convId, m.timestamp, raw)
            if (seen(key)) continue
            var phone: String? = null
            if (phoneKeyed) {
                // Voice/WhatsApp put the peer's number in the sender or title when there is no contact name.
                // O4: ids, then the sender line, then the title; none -> sender.name only (relay drops it).
                phone = phoneFromIds ?: PhoneNumbers.normalize(name) ?: phoneFromTitle
                if (phone != null) peerPhone = phone
            }
            val tsSec = if (m.timestamp > 0) m.timestamp / 1000 else nowSec
            events += BridgeEvent(
                id = eventId(convId, m.timestamp, raw),
                source = source,
                conversation = Conversation(id = Bounds.convId(convId), title = s.conversationTitle?.let { Bounds.cp(it, Bounds.NAME_CP) }, isGroup = s.isGroup),
                sender = Sender(name = Bounds.cp(name.ifEmpty { s.conversationTitle ?: "?" }, Bounds.NAME_CP), phone = phone),
                text = Bounds.cp(text, Bounds.TEXT_CP),
                ts = tsSec,
                attachments = attachments,
            )
            keys += key
        }
        return Mapped(events, keys, peerPhone)
    }

    /** WA3: `~ Name` / `~Name` (an unsaved WhatsApp member's push name) -> `Name`. */
    fun stripWaTilde(name: String): String =
        if (name.startsWith("~")) name.removePrefix("~").trim() else name

    /**
     * WA6: `📷 Photo` -> (image, ""); `📷 beach day` -> (image, "beach day"); `GIF` -> (image, "");
     * `📄 report.pdf` -> (file, "report.pdf"). Null when the line is ordinary text.
     */
    fun waMedia(text: String): Pair<String, String>? {
        val t = text.trim()
        for (m in Targets.WA_MEDIA) {
            if (m.label == null) {
                if (t.equals(m.prefix, ignoreCase = true)) return m.kind to ""
                continue
            }
            if (!t.startsWith(m.prefix)) continue
            val rest = t.removePrefix(m.prefix).trim()
            return m.kind to (if (rest.isEmpty() || rest.equals(m.label, ignoreCase = true)) "" else rest)
        }
        return null
    }

    /** Event id, stable for the same message so a relay retry is `duplicate` (decision 5). */
    fun eventId(convId: String, timestamp: Long, text: String): String {
        val digest = java.security.MessageDigest.getInstance("SHA-256")
            .digest("$convId|$timestamp|$text".toByteArray())
        return "n_" + digest.take(12).joinToString("") { "%02x".format(it) }
    }
}

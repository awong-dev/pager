package app.kidpager.bridge

/**
 * Decision 13 (listener bullet) and decision 5: the pure mapping from a MessagingStyle
 * notification (already flattened into [NotificationSnapshot] by the listener) to
 * `BridgeEvent`s. Unit-tested in app/src/test; the listener only does the Android calls.
 * WhatsApp (WA2/WA3/WA6, 9 Oct 2026): DM phone from the JID, group sender `~ ` stripping,
 * "You" lines skipped, media placeholders mapped to `attachments`. L8 (10 Oct 2026): a DM is
 * keyed by its JID whatever the shape (`@s.whatsapp.net` or `@lid`); a phone JID also sets
 * `conversation.link` to the `wa.me` chat link so the tier-2 send can open the chat directly.
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
 * `peerPhone`: the DM peer's number when the source is phone-keyed (Voice, WhatsApp DM). The
 * listener maps phone -> conversation id for Voice only, whose tier-1 replies can arrive with
 * just `to.phone`; a WhatsApp send carries the conversation id (and `link` for a phone JID).
 */
data class Mapped(val events: List<BridgeEvent>, val dedupKeys: List<String>, val peerPhone: String?)

object NotificationMapper {
    /** WA6: WhatsApp posts the bridge account's own group lines as sender "You". */
    private const val WA_SELF = "You"

    /** What the listener does with a notification, from [conversationKey]. */
    sealed class ConversationKey {
        /** Spool under this conversation id. */
        data class Id(val id: String) : ConversationKey()
        /** `FLAG_GROUP_SUMMARY` ("N messages from M chats"): never a conversation. */
        object GroupSummary : ConversationKey()
        /** No `shortcutId` and the package does not get the `pkg|id` fallback: a roll-up, dropped. */
        object NoId : ConversationKey()
    }

    /**
     * The conversation-id rule (decision 13, tightened 10 Oct 2026 after Google Chat posted a
     * roll-up of several recent messages with no `shortcutId` and id 0, which the `pkg|id`
     * fallback turned into a new "Untitled conversation" on the relay): group summary -> skip;
     * `shortcutId` present -> that id; otherwise `pkg|id` only for
     * [Targets.PKG_ID_FALLBACK_PACKAGES] (Voice), and drop for Chat and WhatsApp.
     */
    fun conversationKey(pkg: String, id: Int, shortcutId: String?, groupSummary: Boolean): ConversationKey {
        if (groupSummary) return ConversationKey.GroupSummary
        shortcutId?.takeIf { it.isNotBlank() }?.let { return ConversationKey.Id(it) }
        return if (pkg in Targets.PKG_ID_FALLBACK_PACKAGES) ConversationKey.Id("$pkg|$id") else ConversationKey.NoId
    }

    /** [conversationKey] for a flattened snapshot (never a group summary); null means drop. */
    fun conversationId(s: NotificationSnapshot): String? =
        (conversationKey(s.pkg, s.id, s.shortcutId, groupSummary = false) as? ConversationKey.Id)?.id

    /**
     * Maps one notification to events for every message not yet seen. `seen(key)` answers the
     * Room dedup table (keyed by conversation, 7 days); `recentDup(event)` answers the in-memory
     * [RecentMessages] window, which catches the same line re-posted under another key (an
     * edited/updated notification, a roll-up) and is consulted after `seen`. The returned
     * `dedupKeys` are inserted after the events are spooled. Nothing is mapped when
     * [conversationId] is null.
     */
    fun map(
        s: NotificationSnapshot,
        seen: (String) -> Boolean,
        nowSec: Long = System.currentTimeMillis() / 1000,
        recentDup: (BridgeEvent) -> Boolean = { false },
    ): Mapped {
        val source = Targets.sourceFor(s.pkg) ?: return Mapped(emptyList(), emptyList(), null)
        val convId = conversationId(s) ?: return Mapped(emptyList(), emptyList(), null)
        val phoneKeyed = source == Targets.SOURCE_GVOICE || (source == Targets.SOURCE_WHATSAPP && !s.isGroup)
        // O4 / WA2: the peer's number from the shortcut id / data URI when present, before the title.
        val phoneFromIds = when {
            source == Targets.SOURCE_GVOICE -> PhoneNumbers.extractE164(s.shortcutId) ?: PhoneNumbers.extractE164(s.dataUri)
            // WA2: `<digits>@s.whatsapp.net` -> +digits; `<digits>@lid` carries no number (L8: that is
            // normal, the JID is still the conversation id and the relay keys on it).
            phoneKeyed -> Targets.phoneFromWaJid(s.shortcutId) ?: PhoneNumbers.extractE164(s.dataUri)
            else -> null
        }
        val phoneFromTitle = if (phoneKeyed) PhoneNumbers.normalize(s.conversationTitle) else null
        // L8: a phone-JID DM gets the `wa.me` chat link, the deep-link tier 2 for the send; a LID or a group has none.
        val link = if (source == Targets.SOURCE_WHATSAPP && !s.isGroup) Targets.phoneFromWaJid(s.shortcutId)?.let { Targets.waLink(it) } else null
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
            val event = BridgeEvent(
                id = eventId(convId, m.timestamp, raw),
                source = source,
                conversation = Conversation(id = Bounds.convId(convId), title = s.conversationTitle?.let { Bounds.cp(it, Bounds.NAME_CP) }, isGroup = s.isGroup, link = link),
                sender = Sender(name = Bounds.cp(name.ifEmpty { s.conversationTitle ?: "?" }, Bounds.NAME_CP), phone = phone),
                text = Bounds.cp(text, Bounds.TEXT_CP),
                ts = tsSec,
                attachments = attachments,
            )
            if (recentDup(event)) continue
            events += event
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

/**
 * Cross-conversation dedup (10 Oct 2026): a bounded window of `(pkg, sender, text, ts)` tuples
 * spooled in the last [ttlMs]. The Room table keys on the conversation id, so a message that an
 * app re-posts under another key (an updated/edited notification, a roll-up that slipped past
 * [NotificationMapper.conversationKey]) would double-send without this. Pure; unit-tested.
 */
class RecentMessages(private val ttlMs: Long = TTL_MS, private val maxEntries: Int = MAX_ENTRIES) {
    companion object {
        const val TTL_MS = 5L * 60 * 1000
        const val MAX_ENTRIES = 256
    }

    data class Key(val pkg: String, val sender: String, val text: String, val ts: Long)

    /** Insertion-ordered so the eldest tuple is evicted first. */
    private val seen = LinkedHashMap<Key, Long>()

    /** True when `key` was recorded within the window; otherwise records it (at `now`) and returns false. */
    @Synchronized
    fun isDuplicate(key: Key, now: Long = System.currentTimeMillis()): Boolean {
        val it = seen.entries.iterator()
        while (it.hasNext()) {
            if (now - it.next().value >= ttlMs) it.remove() else break
        }
        if (seen.containsKey(key)) return true
        seen[key] = now
        while (seen.size > maxEntries) seen.remove(seen.keys.first())
        return false
    }

    fun isDuplicate(pkg: String, e: BridgeEvent, now: Long = System.currentTimeMillis()): Boolean =
        isDuplicate(Key(pkg, e.sender.name, e.text, e.ts), now)

    @Synchronized
    fun size(): Int = seen.size
}

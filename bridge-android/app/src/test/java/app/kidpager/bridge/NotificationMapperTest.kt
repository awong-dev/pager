package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** A3: MessagingStyle snapshot -> BridgeEvent mapping, conversation ids, Voice numbers (O4), dedup, bounds; A9: WhatsApp (WA2/WA3/WA6). */
class NotificationMapperTest {
    private fun snap(
        pkg: String = Targets.GCHAT_PKG,
        shortcutId: String? = "spaces/AAAA",
        title: String? = "Soccer carpool",
        isGroup: Boolean = true,
        self: String? = "Kid",
        dataUri: String? = null,
        messages: List<SnapshotMessage>,
    ) = NotificationSnapshot(pkg, "0|$pkg|7|null|10123", 7, shortcutId, dataUri, title, isGroup, self, messages)

    private val noneSeen: (String) -> Boolean = { false }

    @Test
    fun chatGroupMessagesBecomeGchatEvents() {
        val m = NotificationMapper.map(
            snap(messages = listOf(
                SnapshotMessage("Dana P", "practice moved to 5", 1_700_000_000_000L, false),
                SnapshotMessage("Lee", "ok", 1_700_000_001_000L, false),
            )),
            noneSeen,
        )
        assertEquals(2, m.events.size)
        val e = m.events[0]
        assertEquals("gchat", e.source)
        assertEquals("spaces/AAAA", e.conversation.id)
        assertEquals("Soccer carpool", e.conversation.title)
        assertTrue(e.conversation.isGroup)
        assertEquals("Dana P", e.sender.name)
        assertNull(e.sender.phone)
        assertEquals("practice moved to 5", e.text)
        assertEquals(1_700_000_000L, e.ts)
        assertNull(e.kind)
        assertTrue(e.id.matches(Regex("^[A-Za-z0-9_-]{1,64}$")))
        assertEquals(2, m.dedupKeys.size)
    }

    // ---- conversation-id rule (10 Oct 2026: Chat roll-up with no shortcutId must not become `pkg|0`) ----

    @Test
    fun conversationKeyUsesShortcutIdWhenPresent() {
        assertEquals(NotificationMapper.ConversationKey.Id("7LRis4AAAAE"), NotificationMapper.conversationKey(Targets.GCHAT_PKG, 0, "7LRis4AAAAE", groupSummary = false))
        assertEquals(NotificationMapper.ConversationKey.Id("15551234567@s.whatsapp.net"), NotificationMapper.conversationKey(Targets.WHATSAPP_PKG, 3, "15551234567@s.whatsapp.net", groupSummary = false))
        assertEquals(NotificationMapper.ConversationKey.Id("t.+15551234567"), NotificationMapper.conversationKey(Targets.GVOICE_PKG, 3, "t.+15551234567", groupSummary = false))
        assertEquals("7LRis4AAAAE", NotificationMapper.conversationId(snap(shortcutId = "7LRis4AAAAE", messages = emptyList())))
    }

    @Test
    fun conversationKeySkipsGroupSummariesEvenWithAShortcutId() {
        assertEquals(NotificationMapper.ConversationKey.GroupSummary, NotificationMapper.conversationKey(Targets.GCHAT_PKG, 0, null, groupSummary = true))
        assertEquals(NotificationMapper.ConversationKey.GroupSummary, NotificationMapper.conversationKey(Targets.WHATSAPP_PKG, 1, "x@g.us", groupSummary = true))
        assertEquals(NotificationMapper.ConversationKey.GroupSummary, NotificationMapper.conversationKey(Targets.GVOICE_PKG, 1, null, groupSummary = true))
    }

    @Test
    fun chatAndWhatsappWithoutShortcutIdAreDropped() {
        for (pkg in listOf(Targets.GCHAT_PKG, Targets.WHATSAPP_PKG, Targets.WHATSAPP_BUSINESS_PKG)) {
            assertEquals(pkg, NotificationMapper.ConversationKey.NoId, NotificationMapper.conversationKey(pkg, 0, null, groupSummary = false))
            assertEquals(pkg, NotificationMapper.ConversationKey.NoId, NotificationMapper.conversationKey(pkg, 7, "  ", groupSummary = false))
        }
        assertNull(NotificationMapper.conversationId(snap(shortcutId = null, messages = emptyList())))
        assertNull(NotificationMapper.conversationId(snap(pkg = Targets.WHATSAPP_PKG, shortcutId = "", messages = emptyList())))
        // The bench shape: id 0, two recent messages, no shortcut id -> nothing is spooled.
        val rollUp = NotificationMapper.map(
            NotificationSnapshot(Targets.GCHAT_PKG, "0|${Targets.GCHAT_PKG}|0|null|10123", 0, null, null, null, false, "Kid", listOf(
                SnapshotMessage("Dana P", "hello", 1L, false),
                SnapshotMessage("Dana P", "again", 2L, false))),
            noneSeen,
        )
        assertTrue(rollUp.events.isEmpty())
        assertTrue(rollUp.dedupKeys.isEmpty())
    }

    @Test
    fun voiceWithoutShortcutIdKeepsThePackageIdFallback() {
        assertEquals(NotificationMapper.ConversationKey.Id("${Targets.GVOICE_PKG}|7"), NotificationMapper.conversationKey(Targets.GVOICE_PKG, 7, null, groupSummary = false))
        assertEquals("${Targets.GVOICE_PKG}|7", NotificationMapper.conversationId(snap(pkg = Targets.GVOICE_PKG, shortcutId = "  ", messages = emptyList())))
        val m = NotificationMapper.map(
            snap(pkg = Targets.GVOICE_PKG, shortcutId = null, title = "Grandma", isGroup = false,
                messages = listOf(SnapshotMessage("(555) 987-6543", "hello", 1L, false))),
            noneSeen,
        )
        assertEquals("${Targets.GVOICE_PKG}|7", m.events[0].conversation.id)
        assertEquals("+15559876543", m.events[0].sender.phone)
    }

    // ---- recent-message dedup across conversation keys (10 Oct 2026) ----

    @Test
    fun recentDupPredicateDropsTheEventAndItsDedupKey() {
        val msgs = listOf(SnapshotMessage("Dana P", "hello", 1_700_000_000_000L, false), SnapshotMessage("Lee", "ok", 1_700_000_001_000L, false))
        val recent = RecentMessages()
        val first = NotificationMapper.map(snap(messages = msgs), noneSeen, recentDup = { recent.isDuplicate(Targets.GCHAT_PKG, it, now = 0L) })
        assertEquals(2, first.events.size)
        assertEquals(2, first.dedupKeys.size)
        // Same lines re-posted under another conversation key: a different Room key, but the same tuple.
        val second = NotificationMapper.map(snap(shortcutId = "other", messages = msgs), noneSeen, recentDup = { recent.isDuplicate(Targets.GCHAT_PKG, it, now = 1_000L) })
        assertTrue(second.events.isEmpty())
        assertTrue(second.dedupKeys.isEmpty())
        // A new line in the same post still goes through.
        val third = NotificationMapper.map(snap(messages = msgs + SnapshotMessage("Lee", "new", 1_700_000_002_000L, false)), noneSeen, recentDup = { recent.isDuplicate(Targets.GCHAT_PKG, it, now = 2_000L) })
        assertEquals(listOf("new"), third.events.map { it.text })
    }

    @Test
    fun recentMessagesExpireAfterTheWindowAndAreBounded() {
        val r = RecentMessages(ttlMs = 1_000L, maxEntries = 3)
        val k = RecentMessages.Key("p", "s", "t", 1L)
        assertFalse(r.isDuplicate(k, now = 0L))
        assertTrue(r.isDuplicate(k, now = 999L))
        assertFalse(r.isDuplicate(k, now = 1_000L))
        // Another package, sender, text or timestamp is a different message.
        assertFalse(r.isDuplicate(k.copy(pkg = "q"), now = 1_000L))
        assertFalse(r.isDuplicate(k.copy(sender = "x"), now = 1_000L))
        assertFalse(r.isDuplicate(k.copy(text = "u"), now = 1_000L))
        assertEquals(3, r.size())
        assertFalse(r.isDuplicate(k.copy(ts = 2L), now = 1_000L))
        assertEquals(3, r.size())
        // The eldest (k) was evicted by the size cap, so it is new again.
        assertFalse(r.isDuplicate(k, now = 1_000L))
    }

    @Test
    fun ownMessagesAndEmptyTextAreSkipped() {
        val m = NotificationMapper.map(
            snap(messages = listOf(
                SnapshotMessage(null, "my own reply", 1L, true),
                SnapshotMessage("Kid", "also mine", 2L, false),
                SnapshotMessage("Dana P", "   ", 3L, false),
                SnapshotMessage("Dana P", "real", 4L, false),
            )),
            noneSeen,
        )
        assertEquals(listOf("real"), m.events.map { it.text })
    }

    @Test
    fun dedupSkipsSeenKeysAndKeyIsStable() {
        val msg = SnapshotMessage("Dana P", "hi", 5L, false)
        val key = Dedup.key("spaces/AAAA", 5L, "hi")
        val first = NotificationMapper.map(snap(messages = listOf(msg)), noneSeen)
        assertEquals(listOf(key), first.dedupKeys)
        val second = NotificationMapper.map(snap(messages = listOf(msg)), seen = { it == key })
        assertTrue(second.events.isEmpty())
        assertEquals(first.events[0].id, NotificationMapper.map(snap(messages = listOf(msg)), noneSeen).events[0].id)
    }

    @Test
    fun voiceNumberFromShortcutBeforeSenderBeforeTitle() {
        val fromShortcut = NotificationMapper.map(
            snap(pkg = Targets.GVOICE_PKG, shortcutId = "t.+15551234567", title = "Grandma", isGroup = false,
                messages = listOf(SnapshotMessage("Grandma", "hello", 1L, false))),
            noneSeen,
        )
        assertEquals("gvoice", fromShortcut.events[0].source)
        assertEquals("+15551234567", fromShortcut.events[0].sender.phone)
        assertEquals("+15551234567", fromShortcut.peerPhone)

        val fromSender = NotificationMapper.map(
            snap(pkg = Targets.GVOICE_PKG, shortcutId = "thread-abc", title = "Messages", isGroup = false,
                messages = listOf(SnapshotMessage("(555) 987-6543", "hello", 1L, false))),
            noneSeen,
        )
        assertEquals("+15559876543", fromSender.events[0].sender.phone)

        val fromTitle = NotificationMapper.map(
            snap(pkg = Targets.GVOICE_PKG, shortcutId = "thread-abc", title = "+1 555-111-2222", isGroup = false,
                messages = listOf(SnapshotMessage("Me", "hello", 1L, false), SnapshotMessage("Them", "x", 2L, false))),
            noneSeen,
        )
        assertEquals("+15551112222", fromTitle.events[0].sender.phone)
    }

    @Test
    fun voiceWithoutResolvableNumberKeepsNameOnly() {
        val m = NotificationMapper.map(
            snap(pkg = Targets.GVOICE_PKG, shortcutId = "thread-abc", title = "Grandma", isGroup = false,
                messages = listOf(SnapshotMessage("Grandma", "hello", 1L, false))),
            noneSeen,
        )
        assertEquals(1, m.events.size)
        assertNull(m.events[0].sender.phone)
        assertEquals("Grandma", m.events[0].sender.name)
        assertNull(m.peerPhone)
    }

    @Test
    fun unknownPackageMapsToNothing() {
        val m = NotificationMapper.map(snap(pkg = "com.example.other", messages = listOf(SnapshotMessage("x", "y", 1L, false))), noneSeen)
        assertTrue(m.events.isEmpty())
    }

    @Test
    fun boundsAreApplied() {
        val long = "x".repeat(2000)
        val m = NotificationMapper.map(
            snap(title = "t".repeat(300), messages = listOf(SnapshotMessage("n".repeat(300), long, 1L, false))),
            noneSeen,
        )
        assertEquals(1600, m.events[0].text.length)
        assertEquals(200, m.events[0].sender.name.length)
        assertEquals(200, m.events[0].conversation.title!!.length)
        assertFalse(Bounds.convId("c".repeat(600)).toByteArray().size > 512)
        assertEquals("ab", Bounds.cp("ab", 5))
    }

    // ---- WhatsApp (WA2/WA3/WA6) ----

    private fun wa(
        shortcutId: String? = "15551234567@s.whatsapp.net",
        title: String? = "+1 555-123-4567",
        isGroup: Boolean = false,
        messages: List<SnapshotMessage>,
    ) = snap(pkg = Targets.WHATSAPP_PKG, shortcutId = shortcutId, title = title, isGroup = isGroup, self = "You", messages = messages)

    @Test
    fun whatsappDmPhoneComesFromTheJid() {
        val m = NotificationMapper.map(wa(title = "Grandma", messages = listOf(SnapshotMessage("Grandma", "hello", 1L, false))), noneSeen)
        assertEquals(1, m.events.size)
        val e = m.events[0]
        assertEquals("whatsapp", e.source)
        assertEquals("15551234567@s.whatsapp.net", e.conversation.id)
        assertFalse(e.conversation.isGroup)
        assertEquals("+15551234567", e.sender.phone)
        assertEquals("Grandma", e.sender.name)
        assertEquals("+15551234567", m.peerPhone)
        assertNull(e.attachments)
    }

    @Test
    fun whatsappBusinessMapsTheSame() {
        val m = NotificationMapper.map(
            snap(pkg = Targets.WHATSAPP_BUSINESS_PKG, shortcutId = "447700900123@s.whatsapp.net", title = "x", isGroup = false, self = "You",
                messages = listOf(SnapshotMessage("x", "hi", 1L, false))),
            noneSeen,
        )
        assertEquals("whatsapp", m.events[0].source)
        assertEquals("+447700900123", m.events[0].sender.phone)
    }

    /** A10 / L8: a LID DM is a normal conversation keyed by its JID; only a phone JID gets the `wa.me` link. */
    @Test
    fun whatsappDmLinkOnlyForPhoneJid() {
        val lid = NotificationMapper.map(wa(shortcutId = "123456789012345@lid", title = "Grandma", messages = listOf(SnapshotMessage("Grandma", "hello", 1L, false))), noneSeen)
        assertEquals(1, lid.events.size)
        assertEquals("123456789012345@lid", lid.events[0].conversation.id)
        assertNull(lid.events[0].sender.phone)
        assertNull(lid.events[0].conversation.link)
        assertEquals("Grandma", lid.events[0].sender.name)

        val jid = NotificationMapper.map(wa(title = "Grandma", messages = listOf(SnapshotMessage("Grandma", "hello", 1L, false))), noneSeen)
        assertEquals("https://wa.me/15551234567", jid.events[0].conversation.link)

        val group = NotificationMapper.map(
            wa(shortcutId = "120363000000000000@g.us", title = "Soccer parents", isGroup = true, messages = listOf(SnapshotMessage("~ Dana P", "hi", 1L, false))),
            noneSeen,
        )
        assertNull(group.events[0].conversation.link)
    }

    @Test
    fun whatsappGroupStripsTildeAndSkipsYou() {
        val m = NotificationMapper.map(
            wa(shortcutId = "120363000000000000@g.us", title = "Soccer parents", isGroup = true, messages = listOf(
                SnapshotMessage("~ Dana P", "practice moved", 1L, false),
                SnapshotMessage("~Lee", "ok", 2L, false),
                SnapshotMessage("You", "see you there", 3L, false),
                SnapshotMessage("+1 555-222-3333", "who is this", 4L, false))),
            noneSeen,
        )
        assertEquals(listOf("Dana P", "Lee", "+1 555-222-3333"), m.events.map { it.sender.name })
        assertTrue(m.events.all { it.conversation.isGroup && it.conversation.id == "120363000000000000@g.us" && it.conversation.title == "Soccer parents" })
        // Groups are not phone-keyed: no sender.phone even when the member shows as a number, and no peer phone.
        assertTrue(m.events.all { it.sender.phone == null })
        assertNull(m.peerPhone)
    }

    @Test
    fun whatsappMediaPlaceholdersBecomeAttachments() {
        val m = NotificationMapper.map(
            wa(title = "Grandma", messages = listOf(
                SnapshotMessage("Grandma", "📷 Photo", 1L, false),
                SnapshotMessage("Grandma", "📷 beach day", 2L, false),
                SnapshotMessage("Grandma", "🎥 Video", 3L, false),
                SnapshotMessage("Grandma", "🎤 Voice message", 4L, false),
                SnapshotMessage("Grandma", "🎵 Audio", 5L, false),
                SnapshotMessage("Grandma", "📄 report.pdf", 6L, false),
                SnapshotMessage("Grandma", "📍 Location", 7L, false),
                SnapshotMessage("Grandma", "👤 Contact", 8L, false),
                SnapshotMessage("Grandma", "GIF", 9L, false),
                SnapshotMessage("Grandma", "Sticker", 10L, false),
                SnapshotMessage("Grandma", "plain text", 11L, false))),
            noneSeen,
        )
        assertEquals(11, m.events.size)
        val kinds = m.events.map { it.attachments?.map { a -> a.kind } }
        assertEquals(listOf(listOf("image"), listOf("image"), listOf("video"), listOf("audio"), listOf("audio"), listOf("file"), listOf("file"), listOf("file"), listOf("image"), listOf("image"), null), kinds)
        assertEquals(listOf("", "beach day", "", "", "", "report.pdf", "", "", "", "", "plain text"), m.events.map { it.text })
        // Dedup keys and ids use the original line, so the placeholder and a captioned photo differ.
        assertEquals(11, m.dedupKeys.toSet().size)
        assertEquals(11, m.events.map { it.id }.toSet().size)
    }

    @Test
    fun whatsappHelpersArePure() {
        assertEquals("image" to "", NotificationMapper.waMedia("📷 Photo"))
        assertEquals("image" to "", NotificationMapper.waMedia("  📷 photo "))
        assertEquals("image" to "cap", NotificationMapper.waMedia("📷 cap"))
        assertNull(NotificationMapper.waMedia("a 📷 in the middle"))
        assertNull(NotificationMapper.waMedia("gift"))
        assertEquals("Dana", NotificationMapper.stripWaTilde("~ Dana"))
        assertEquals("Dana", NotificationMapper.stripWaTilde("~Dana"))
        assertEquals("Dana ~ P", NotificationMapper.stripWaTilde("Dana ~ P"))
    }

    @Test
    fun missingTimestampUsesNow() {
        val m = NotificationMapper.map(snap(messages = listOf(SnapshotMessage("a", "b", 0L, false))), noneSeen, nowSec = 1234L)
        assertEquals(1234L, m.events[0].ts)
    }
}

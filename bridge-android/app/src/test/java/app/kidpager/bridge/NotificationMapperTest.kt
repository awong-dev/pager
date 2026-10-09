package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** A3: MessagingStyle snapshot -> BridgeEvent mapping, conversation ids, Voice numbers (O4), dedup, bounds. */
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

    @Test
    fun conversationIdFallsBackToPackageAndId() {
        assertEquals("${Targets.GCHAT_PKG}|7", NotificationMapper.conversationId(snap(shortcutId = null, messages = emptyList())))
        assertEquals("${Targets.GCHAT_PKG}|7", NotificationMapper.conversationId(snap(shortcutId = "  ", messages = emptyList())))
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
        assertEquals("+15551234567", fromShortcut.voicePeerPhone)

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
        assertNull(m.voicePeerPhone)
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

    @Test
    fun missingTimestampUsesNow() {
        val m = NotificationMapper.map(snap(messages = listOf(SnapshotMessage("a", "b", 0L, false))), noneSeen, nowSec = 1234L)
        assertEquals(1234L, m.events[0].ts)
    }
}

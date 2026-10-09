package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** A4: an inbound SMS becomes one `sms` event keyed by the normalised sender. */
class SmsEventTest {
    @Test
    fun smsEventShape() {
        val e = SmsReceiver.event("(555) 123-4567", "hi kid", 1_700_000_000_500L)
        assertEquals("sms", e.source)
        assertEquals("+15551234567", e.sender.phone)
        assertEquals("+15551234567", e.conversation.id)
        assertEquals(false, e.conversation.isGroup)
        assertEquals("hi kid", e.text)
        assertEquals(1_700_000_000L, e.ts)
        assertTrue(e.id.startsWith("s_"))
        assertEquals(e.id, SmsReceiver.event("+15551234567", "hi kid", 1_700_000_000_500L).id)
    }

    @Test
    fun eventJsonOmitsNulls() {
        val e = SmsReceiver.event("+15551234567", "x", 1000L)
        val json = WireJson.encodeToString(BridgeEvent.serializer(), e)
        assertTrue(json.contains("\"source\":\"sms\""))
        assertTrue(!json.contains("\"kind\""))
        assertTrue(!json.contains("\"attachments\""))
        assertTrue(json.contains("\"isGroup\":false"))
    }
}

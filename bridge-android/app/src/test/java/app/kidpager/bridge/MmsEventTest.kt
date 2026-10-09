package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** A4 / A8: the pure parsed-PDU -> `sms` event step of MmsReceiver. */
class MmsEventTest {
    private fun pdu(parts: List<MmsPdu.Part>) =
        MmsPdu.Pdu(MmsPdu.MESSAGE_TYPE_RETRIEVE_CONF, "+15551234567", "t", null, "application/vnd.wap.multipart.related", parts)

    @Test
    fun textAndAttachmentKinds() {
        val e = MmsReceiver.event(
            "15551234567",
            pdu(listOf(
                MmsPdu.Part("text/plain; charset=utf-8", "look".toByteArray(), null),
                MmsPdu.Part("image/jpeg", byteArrayOf(1), "a.jpg"),
                MmsPdu.Part("application/smil", "<smil/>".toByteArray(), null),
            )),
            1_700_000_000_250L,
        )
        assertEquals("sms", e.source)
        assertEquals("+15551234567", e.sender.phone)
        assertEquals("+15551234567", e.conversation.id)
        assertEquals("look", e.text)
        assertEquals(listOf("image"), e.attachments!!.map { it.kind })
        assertEquals(1_700_000_000L, e.ts)
        assertTrue(e.id.startsWith("m_"))
    }

    @Test
    fun mediaOnlyHasEmptyTextAndNoAttachmentsKeyWhenNone() {
        val media = MmsReceiver.event("+15551234567", pdu(listOf(MmsPdu.Part("video/mp4", byteArrayOf(1), null))), 1000L)
        assertEquals("", media.text)
        assertEquals(listOf("video"), media.attachments!!.map { it.kind })
        val textOnly = MmsReceiver.event("+15551234567", pdu(listOf(MmsPdu.Part("text/plain", "x".toByteArray(), null))), 1000L)
        assertNull(textOnly.attachments)
    }
}

package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.ByteArrayOutputStream

/** A4: M-Notification.ind and M-Retrieve.conf parsing (text part + attachment kinds). */
class MmsPduTest {
    private fun ByteArrayOutputStream.text(s: String) { write(s.toByteArray()); write(0) }
    private fun ByteArrayOutputStream.uintvar(v: Int) {
        val bytes = ArrayList<Int>()
        var x = v
        do { bytes.add(0, x and 0x7F); x = x shr 7 } while (x > 0)
        for (i in bytes.indices) write(if (i < bytes.size - 1) bytes[i] or 0x80 else bytes[i])
    }
    private fun ByteArrayOutputStream.from(addr: String) {
        val enc = ByteArrayOutputStream().apply { write(0x80); text(addr) }.toByteArray()
        write(0x89); write(enc.size); write(enc)
    }

    @Test
    fun parsesNotificationInd() {
        val b = ByteArrayOutputStream().apply {
            write(0x8C); write(0x82)                   // message type: notification-ind
            write(0x98); text("txn-1")                 // transaction id
            write(0x8D); write(0x92)                   // version 1.2 (short int)
            from("+15551234567/TYPE=PLMN")
            write(0x8A); write(0x80)                   // message class: personal
            write(0x8E); write(0x02); write(0x01); write(0x00) // size: long integer 256
            write(0x88); write(0x05); write(0x81); write(0x03); write(0x00); write(0x0E); write(0x10) // expiry (skipped)
            write(0x83); text("http://mmsc.example/abc") // content location
        }.toByteArray()
        val p = MmsPdu.parse(b)
        assertEquals(MmsPdu.MESSAGE_TYPE_NOTIFICATION_IND, p.messageType)
        assertEquals("txn-1", p.transactionId)
        assertEquals("+15551234567", p.from)
        assertEquals("http://mmsc.example/abc", p.contentLocation)
        assertNull(p.contentType)
    }

    @Test
    fun parsesRetrieveConfWithTextAndImage() {
        val textData = "hello from mms".toByteArray()
        val imgData = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47)
        val smil = "<smil/>".toByteArray()
        val b = ByteArrayOutputStream().apply {
            write(0x8C); write(0x84)                   // retrieve-conf
            write(0x98); text("txn-2")
            from("15559876543")
            // Content-Type general form: application/vnd.wap.multipart.related (0xB3) with start param
            val ct = ByteArrayOutputStream().apply { write(0xB3); write(0x8E); text("<smil>") }.toByteArray()
            write(0x84); write(ct.size); write(ct)
            uintvar(3)
            // part 1: text/plain; charset=utf-8 (general form)
            val h1 = ByteArrayOutputStream().apply { write(0x03); write(0x83); write(0x81); write(0xEA) }.toByteArray()
            uintvar(h1.size); uintvar(textData.size); write(h1); write(textData)
            // part 2: image/png constrained
            val h2 = byteArrayOf(0xA0.toByte())
            uintvar(h2.size); uintvar(imgData.size); write(h2); write(imgData)
            // part 3: application/smil as a text content type
            val h3 = ByteArrayOutputStream().apply { text("application/smil") }.toByteArray()
            uintvar(h3.size); uintvar(smil.size); write(h3); write(smil)
        }.toByteArray()
        val p = MmsPdu.parse(b)
        assertEquals(MmsPdu.MESSAGE_TYPE_RETRIEVE_CONF, p.messageType)
        assertEquals("15559876543", p.from)
        assertEquals("application/vnd.wap.multipart.related", p.contentType)
        assertEquals(3, p.parts.size)
        assertEquals("hello from mms", p.text)
        assertEquals(listOf("image"), p.attachments.map { it.kind })
    }

    @Test
    fun attachmentKinds() {
        assertEquals("image", attachmentKind("image/jpeg"))
        assertEquals("video", attachmentKind("video/mp4"))
        assertEquals("audio", attachmentKind("audio/amr"))
        assertEquals("file", attachmentKind("application/pdf"))
        assertEquals("file", attachmentKind(null))
    }
}

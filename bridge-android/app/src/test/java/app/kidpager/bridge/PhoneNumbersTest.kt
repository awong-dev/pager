package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** A3/A4: phone normalisation shared by the Voice mapper and the SMS receiver. */
class PhoneNumbersTest {
    @Test
    fun normalizesNorthAmericanShapes() {
        assertEquals("+15551234567", PhoneNumbers.normalize("(555) 123-4567"))
        assertEquals("+15551234567", PhoneNumbers.normalize("555.123.4567"))
        assertEquals("+15551234567", PhoneNumbers.normalize("1 555 123 4567"))
        assertEquals("+15551234567", PhoneNumbers.normalize("+1 555-123-4567"))
        assertEquals("+15551234567", PhoneNumbers.normalize("tel:+15551234567"))
    }

    @Test
    fun keepsInternationalPlus() {
        assertEquals("+447700900123", PhoneNumbers.normalize("+44 7700 900123"))
    }

    @Test
    fun rejectsNamesAndShortStrings() {
        assertNull(PhoneNumbers.normalize("Grandma"))
        assertNull(PhoneNumbers.normalize("Dana P"))
        assertNull(PhoneNumbers.normalize("12345"))
        assertNull(PhoneNumbers.normalize(""))
        assertNull(PhoneNumbers.normalize(null))
        assertFalse(PhoneNumbers.looksLikeNumber("Soccer carpool"))
        assertTrue(PhoneNumbers.looksLikeNumber("+15551234567"))
    }

    @Test
    fun extractsFromOpaqueIds() {
        assertEquals("+15551234567", PhoneNumbers.extractE164("t.+15551234567"))
        assertEquals("+15551234567", PhoneNumbers.extractE164("tel:+1 (555) 123-4567"))
        assertNull(PhoneNumbers.extractE164("thread-abc"))
        assertNull(PhoneNumbers.extractE164(null))
    }

    @Test
    fun whatsappJidHelpers() {
        assertEquals("+15551234567", Targets.phoneFromWaJid("15551234567@s.whatsapp.net"))
        assertEquals("+447700900123", Targets.phoneFromWaJid(" 447700900123@s.whatsapp.net "))
        assertNull(Targets.phoneFromWaJid("123456789012345@lid"))
        assertNull(Targets.phoneFromWaJid("120363000000000000@g.us"))
        assertNull(Targets.phoneFromWaJid("12345@s.whatsapp.net"))
        assertNull(Targets.phoneFromWaJid(null))
        assertEquals("15551234567@s.whatsapp.net", Targets.waDmJid("+1 (555) 123-4567"))
        assertTrue(Targets.WA_GROUP_JID.matches("120363000000000000@g.us"))
        assertTrue(Targets.WA_GROUP_JID.matches("15551234567-1600000000@g.us"))
        assertFalse(Targets.WA_GROUP_JID.matches("15551234567@s.whatsapp.net"))
    }

    @Test
    fun voiceThreadLinkEncodesPlus() {
        assertEquals("https://voice.google.com/u/0/messages?itemId=t.%2B15551234567", Targets.voiceThreadLink("+15551234567"))
        assertEquals("t.+15551234567", Targets.conversationIdFromLink(Targets.voiceThreadLink("+15551234567")))
        assertEquals("room/AAAAxyz", Targets.conversationIdFromLink("https://chat.google.com/room/AAAAxyz/abc"))
        assertEquals("dm/BBBB", Targets.conversationIdFromLink("https://mail.google.com/chat/u/0/#chat/dm/BBBB"))
        assertEquals(Targets.GCHAT_PKG, Targets.packageForLink("https://chat.google.com/room/AAAAxyz"))
        assertEquals(Targets.GVOICE_PKG, Targets.packageForLink("https://voice.google.com/u/0/messages"))
        assertNull(Targets.packageForLink("https://example.com/room/x"))
    }
}

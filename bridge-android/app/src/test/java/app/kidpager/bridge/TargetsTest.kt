package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** A9 (WA1/WA4): WhatsApp packages, sources and link parsing in Targets. */
class TargetsTest {
    @Test
    fun whatsappPackagesAreListenedAndMapped() {
        assertTrue(Targets.WHATSAPP_PKG in Targets.LISTEN_PACKAGES)
        assertTrue(Targets.WHATSAPP_BUSINESS_PKG in Targets.LISTEN_PACKAGES)
        assertEquals("whatsapp", Targets.sourceFor(Targets.WHATSAPP_PKG))
        assertEquals("whatsapp", Targets.sourceFor(Targets.WHATSAPP_BUSINESS_PKG))
        assertEquals("gchat", Targets.sourceFor(Targets.GCHAT_PKG))
        assertNull(Targets.sourceFor("com.example"))
    }

    @Test
    fun whatsappLinksResolveToThePackage() {
        assertEquals(Targets.WHATSAPP_PKG, Targets.packageForLink("https://wa.me/15551234567"))
        assertEquals(Targets.WHATSAPP_PKG, Targets.packageForLink("https://api.whatsapp.com/send?phone=15551234567&text=hi"))
        assertEquals(Targets.WHATSAPP_PKG, Targets.packageForLink("whatsapp://send?phone=15551234567"))
        assertNull(Targets.packageForLink("https://chat.whatsapp.com/AbCdEf"))
        assertNull(Targets.packageForLink("https://wa.me.example.com/x"))
        assertEquals(Targets.GVOICE_PKG, Targets.packageForLink("https://voice.google.com/u/0/messages"))
    }

    @Test
    fun waLinkIsDigitsOnly() {
        assertEquals("https://wa.me/15551234567", Targets.waLink("+15551234567"))
        assertEquals("https://wa.me/15551234567", Targets.waLink("+1 (555) 123-4567"))
    }

    @Test
    fun conversationIdFromWhatsappLinkIsTheDmJid() {
        assertEquals("15551234567@s.whatsapp.net", Targets.conversationIdFromLink("https://wa.me/15551234567"))
        assertEquals("15551234567@s.whatsapp.net", Targets.conversationIdFromLink("https://wa.me/+15551234567?text=hi"))
        assertEquals("15551234567@s.whatsapp.net", Targets.conversationIdFromLink("https://api.whatsapp.com/send?phone=15551234567"))
        assertEquals("15551234567@s.whatsapp.net", Targets.conversationIdFromLink("https://api.whatsapp.com/send?text=hi&phone=%2B15551234567"))
        assertEquals("15551234567@s.whatsapp.net", Targets.conversationIdFromLink("whatsapp://send?phone=15551234567"))
        assertNull(Targets.conversationIdFromLink("https://wa.me/"))
        assertEquals("t.+15551234567", Targets.conversationIdFromLink(Targets.voiceThreadLink("+15551234567")))
        assertEquals("room/AAAAxyz", Targets.conversationIdFromLink("https://chat.google.com/room/AAAAxyz/abc"))
    }
}

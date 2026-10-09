package app.kidpager.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** A5 / decision 11: an item is dispatched once, its ack is retried unchanged until accepted. */
class AckTrackerTest {
    @Test
    fun dispatchOncePerItem() {
        val t = AckTracker()
        assertTrue(t.begin("ob_1"))
        assertFalse(t.begin("ob_1"))
        assertEquals(AckTracker.Phase.IN_FLIGHT, t.phase("ob_1"))
        assertTrue(t.pendingAcks().isEmpty())
    }

    @Test
    fun completeThenAckLifecycle() {
        val t = AckTracker()
        t.begin("ob_1")
        assertTrue(t.complete("ob_1", "sent", null, 1))
        assertEquals(listOf("ob_1" to AckTracker.Result("sent", null, 1)), t.pendingAcks())
        // a relay failure keeps the same result pending; nothing changes on retry
        assertEquals(listOf("ob_1" to AckTracker.Result("sent", null, 1)), t.pendingAcks())
        t.acked("ob_1")
        assertTrue(t.pendingAcks().isEmpty())
        assertEquals(AckTracker.Phase.ACKED, t.phase("ob_1"))
        // the next poll still returning the item does not redispatch it
        assertFalse(t.begin("ob_1"))
    }

    @Test
    fun firstResultWins() {
        val t = AckTracker()
        t.begin("ob_2")
        assertTrue(t.complete("ob_2", "failed", "ui_changed", 2))
        assertFalse(t.complete("ob_2", "sent", null, 1))
        assertEquals(AckTracker.Result("failed", "ui_changed", 2), t.pendingAcks().single().second)
    }

    @Test
    fun completeUnknownIsIgnoredAndOldAckedForgotten() {
        val t = AckTracker()
        assertFalse(t.complete("nope", "sent", null, 1))
        t.begin("ob_3", now = 1000L)
        t.complete("ob_3", "sent", null, 1)
        t.acked("ob_3")
        t.forgetAckedBefore(500L)
        assertEquals(1, t.size())
        t.forgetAckedBefore(2000L)
        assertEquals(0, t.size())
        assertTrue(t.begin("ob_3"))
        // in-flight items are never forgotten
        t.forgetAckedBefore(Long.MAX_VALUE)
        assertEquals(1, t.size())
    }
}

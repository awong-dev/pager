package app.kidpager.bridge

/**
 * Decision 11 ("ack per item with tier; re-acks are idempotent") on the phone side, as a pure
 * state machine: an outbox item is dispatched once per process even if the next poll returns it
 * again before the ack lands; its result is kept until the relay accepts the ack, and retried
 * acks carry the same `(state, reason, tier)`. Unit-tested in app/src/test.
 */
class AckTracker {
    enum class Phase { IN_FLIGHT, DONE, ACKED }

    data class Result(val state: String, val reason: String?, val tier: Int)

    private class Entry(var phase: Phase, var result: Result?, val startedAt: Long)

    private val entries = HashMap<String, Entry>()

    /** True when `id` is new (or acked long enough ago to be forgotten); marks it in flight. */
    @Synchronized
    fun begin(id: String, now: Long = System.currentTimeMillis()): Boolean {
        val e = entries[id]
        if (e != null) return false
        entries[id] = Entry(Phase.IN_FLIGHT, null, now)
        return true
    }

    /** Records the outcome; a second completion for the same id is ignored (first result wins). */
    @Synchronized
    fun complete(id: String, state: String, reason: String?, tier: Int): Boolean {
        val e = entries[id] ?: return false
        if (e.phase != Phase.IN_FLIGHT) return false
        e.phase = Phase.DONE
        e.result = Result(state, reason, tier)
        return true
    }

    /** Items whose ack the relay has not accepted yet. */
    @Synchronized
    fun pendingAcks(): List<Pair<String, Result>> =
        entries.filter { it.value.phase == Phase.DONE }.map { it.key to it.value.result!! }

    @Synchronized
    fun acked(id: String) {
        entries[id]?.let { it.phase = Phase.ACKED }
    }

    @Synchronized
    fun phase(id: String): Phase? = entries[id]?.phase

    /** Forgets acked items older than `ttlMs` so a relay re-send after 24 h is not confused with a duplicate. */
    @Synchronized
    fun forgetAckedBefore(cutoff: Long) {
        entries.entries.removeAll { it.value.phase == Phase.ACKED && it.value.startedAt < cutoff }
    }

    @Synchronized
    fun size() = entries.size
}

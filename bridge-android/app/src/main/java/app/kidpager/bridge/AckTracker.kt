package app.kidpager.bridge

/**
 * Decision 11 ("ack per item with tier; re-acks are idempotent") on the phone side, as a pure
 * state machine: an outbox item is dispatched once per process even if the next poll returns it
 * again before the ack lands; its result is kept until the relay accepts the ack, and retried
 * acks carry the same `(state, reason, tier)`. A8 review fix: a DONE entry whose ack keeps
 * failing is purged after [MAX_ACK_ATTEMPTS] or [WINDOW_MS]; a 404 on ack forgets the entry;
 * `begin` accepts an id whose entry is older than the window, so a relay-reissued item is
 * dispatched again. Unit-tested in app/src/test.
 */
class AckTracker(
    private val windowMs: Long = WINDOW_MS,
    private val maxAttempts: Int = MAX_ACK_ATTEMPTS,
) {
    companion object {
        const val WINDOW_MS = 24L * 3600 * 1000
        const val MAX_ACK_ATTEMPTS = 20
    }

    enum class Phase { IN_FLIGHT, DONE, ACKED }

    data class Result(val state: String, val reason: String?, val tier: Int)

    private class Entry(var phase: Phase, var result: Result?, val startedAt: Long, var attempts: Int = 0)

    private val entries = HashMap<String, Entry>()

    /**
     * True when `id` is new, or its entry is not in flight and older than the window (a relay
     * re-issue); marks it in flight.
     */
    @Synchronized
    fun begin(id: String, now: Long = System.currentTimeMillis()): Boolean {
        val e = entries[id]
        if (e != null && (e.phase == Phase.IN_FLIGHT || now - e.startedAt < windowMs)) return false
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

    /** The relay no longer knows the item (404 on ack): drop it; a re-issue is a new begin(). */
    @Synchronized
    fun forget(id: String) { entries.remove(id) }

    /** An ack attempt failed (transport or 5xx); returns the attempt count so far. */
    @Synchronized
    fun ackFailed(id: String): Int {
        val e = entries[id] ?: return 0
        e.attempts += 1
        return e.attempts
    }

    @Synchronized
    fun phase(id: String): Phase? = entries[id]?.phase

    @Synchronized
    fun attempts(id: String): Int = entries[id]?.attempts ?: 0

    /**
     * Drops ACKED entries older than the window and DONE entries that exhausted their ack
     * attempts or aged past the window. Returns the ids of DONE entries given up on.
     */
    @Synchronized
    fun purge(now: Long = System.currentTimeMillis()): List<String> {
        val dropped = ArrayList<String>()
        val it = entries.entries.iterator()
        while (it.hasNext()) {
            val (id, e) = it.next()
            val old = now - e.startedAt >= windowMs
            when (e.phase) {
                Phase.ACKED -> if (old) it.remove()
                Phase.DONE -> if (old || e.attempts >= maxAttempts) { it.remove(); dropped += id }
                Phase.IN_FLIGHT -> {}
            }
        }
        return dropped
    }

    /** Kept for callers that only want the ACKED sweep. */
    @Synchronized
    fun forgetAckedBefore(cutoff: Long) {
        entries.entries.removeAll { it.value.phase == Phase.ACKED && it.value.startedAt < cutoff }
    }

    @Synchronized
    fun size() = entries.size
}

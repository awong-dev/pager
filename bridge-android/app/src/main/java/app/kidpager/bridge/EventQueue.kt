package app.kidpager.bridge

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * A3 ("events batched (debounce 500 ms) to POST /bridge/events") and decision 5 ("the phone
 * retries the batch on a non-2xx"): events are spooled in Room, flushed in batches of at most
 * 50 after a 500 ms quiet period, deleted only after a 2xx. Backoff doubles to 5 min on failure
 * (5xx, 429, network). A 4xx other than 401/429 means the relay rejected the payload itself, so
 * that batch is logged and dropped rather than retried forever.
 */
object EventQueue {
    private const val TAG = "events"
    private const val BATCH = 50
    private const val DEBOUNCE_MS = 500L
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val kicks = Channel<Unit>(Channel.CONFLATED)
    @Volatile private var loop: Job? = null

    fun enqueue(ctx: Context, events: List<BridgeEvent>) {
        if (events.isEmpty()) return
        val db = AppDb.get(ctx)
        val now = System.currentTimeMillis()
        for (e in events) {
            db.pendingEvents().insert(PendingEvent(eventId = e.id, json = WireJson.encodeToString(BridgeEvent.serializer(), e), createdAt = now))
            Log.i(TAG, "spooled ${e.source} conv=${e.conversation.id} from=${if (e.sender.phone != null) Log.redact(e.sender.phone) else e.sender.name} id=${e.id}")
        }
        start(ctx)
        kicks.trySend(Unit)
    }

    /**
     * For BroadcastReceivers: the Room insert must not run on the main thread (AppDb has no
     * allowMainThreadQueries). Runs [enqueue] on the IO scope and calls `onDone` afterwards
     * (the receiver's `goAsync()` result is finished there).
     */
    fun enqueueAsync(ctx: Context, events: List<BridgeEvent>, onDone: () -> Unit = {}) {
        val app = ctx.applicationContext
        scope.launch {
            try {
                enqueue(app, events)
            } catch (e: Exception) {
                Log.e(TAG, "spool failed", e)
            } finally {
                onDone()
            }
        }
    }

    fun start(ctx: Context) {
        if (loop?.isActive == true) return
        synchronized(this) {
            if (loop?.isActive == true) return
            val app = ctx.applicationContext
            loop = scope.launch { run(app) }
        }
    }

    private suspend fun run(ctx: Context) {
        val client = RelayClient(ctx)
        val db = AppDb.get(ctx)
        var backoff = 2_000L
        while (true) {
            kicks.receive()
            delay(DEBOUNCE_MS)
            // drain anything that arrived during the debounce
            withTimeoutOrNull(1) { kicks.receive() }
            while (true) {
                if (!Prefs.isPaired(ctx)) { Log.w(TAG, "not paired; ${db.pendingEvents().count()} events held"); break }
                val rows = db.pendingEvents().oldest(BATCH)
                if (rows.isEmpty()) { backoff = 2_000L; break }
                val events = rows.map { WireJson.decodeFromString(BridgeEvent.serializer(), it.json) }
                try {
                    val resp = client.events(events)
                    db.pendingEvents().delete(rows.map { it.rowId })
                    for (r in resp.results) Log.i(TAG, "relay ${r.id} outcome=${r.outcome}")
                    backoff = 2_000L
                } catch (e: RelayClient.Unauthorized) {
                    break
                } catch (e: RelayClient.HttpError) {
                    if (e.code in 400..499 && e.code != 429) {
                        Log.w(TAG, "relay rejected batch of ${rows.size} (HTTP ${e.code}: ${e.reason()}); dropping ids=${rows.map { it.eventId }}")
                        db.pendingEvents().delete(rows.map { it.rowId })
                        continue
                    }
                    Log.w(TAG, "batch of ${rows.size} failed (HTTP ${e.code}); retry in ${backoff / 1000}s")
                    delay(backoff)
                    backoff = (backoff * 2).coerceAtMost(300_000L)
                    kicks.trySend(Unit)
                    break
                } catch (e: Exception) {
                    Log.w(TAG, "batch of ${rows.size} failed (${e.message}); retry in ${backoff / 1000}s")
                    delay(backoff)
                    backoff = (backoff * 2).coerceAtMost(300_000L)
                    kicks.trySend(Unit)
                    break
                }
            }
        }
    }
}

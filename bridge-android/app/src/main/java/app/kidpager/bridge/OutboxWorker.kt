package app.kidpager.bridge

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * A5 + O2: polls `GET /bridge/outbox` (wait=0) every `Prefs.pollIntervalSec` seconds, at once on
 * an FCM data push (`kick`) or when a heartbeat reports `pending > 0`; dispatches each new item
 * through Dispatcher and acks it with its tier (decision 11), keeping un-acked results in
 * AckTracker until the relay accepts them.
 */
class OutboxWorker(private val ctx: Context, private val scope: CoroutineScope) {
    companion object {
        private const val TAG = "outbox"
        private val kicks = Channel<Unit>(Channel.CONFLATED)
        /** FCM `kind: outbox` or a heartbeat with pending items: poll now. */
        fun kick() { kicks.trySend(Unit) }
    }

    private val client = RelayClient(ctx)
    private val tracker = AckTracker()
    private var job: Job? = null

    fun start() {
        if (job?.isActive == true) return
        job = scope.launch { loop() }
    }

    fun stop() { job?.cancel() }

    private suspend fun loop() {
        while (true) {
            if (Prefs.isPaired(ctx)) {
                try {
                    pollOnce()
                } catch (e: RelayClient.Unauthorized) {
                    Log.w(TAG, "unauthorized; waiting for re-pair")
                } catch (e: Exception) {
                    Log.w(TAG, "poll failed: ${e.message}")
                }
            }
            withTimeoutOrNull(Prefs.pollIntervalSec(ctx) * 1000L) { kicks.receive() }
        }
    }

    private suspend fun pollOnce() {
        flushAcks()
        tracker.forgetAckedBefore(System.currentTimeMillis() - 48L * 3600 * 1000)
        var more = true
        while (more) {
            val items = client.outbox(0).items
            more = false
            for (item in items) {
                if (!tracker.begin(item.id)) continue
                val started = System.currentTimeMillis()
                val out = try {
                    Dispatcher.dispatch(ctx, item)
                } catch (e: Exception) {
                    Log.e(TAG, "dispatch ${item.id} threw", e)
                    Dispatcher.Outcome("failed", "exception", 1)
                }
                tracker.complete(item.id, out.state, out.reason, out.tier)
                Log.i(TAG, "ob=${item.id} kind=${item.kind} src=${item.source ?: "-"} state=${out.state} reason=${out.reason ?: "-"} tier=${out.tier} ms=${System.currentTimeMillis() - started}")
                more = true
            }
            flushAcks()
            // `more` stays true only when something new was dispatched (a full page of already
            // tracked items must not spin); the next poll picks up the rest.
        }
    }

    private fun flushAcks() {
        for ((id, r) in tracker.pendingAcks()) {
            try {
                client.ack(id, r.state, r.reason, r.tier)
                tracker.acked(id)
            } catch (e: RelayClient.Unauthorized) {
                throw e
            } catch (e: RelayClient.HttpError) {
                if (e.code == 404 || e.code == 409 || e.code == 422) {
                    Log.w(TAG, "ack $id rejected ${e.code}; dropping")
                    tracker.acked(id)
                } else Log.w(TAG, "ack $id failed ${e.code}; will retry")
            } catch (e: Exception) {
                Log.w(TAG, "ack $id failed: ${e.message}; will retry")
            }
        }
    }
}

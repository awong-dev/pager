package app.kidpager.bridge

import android.content.Context
import androidx.work.Constraints
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

/**
 * A2: the WorkManager backstop (15-min period, the platform minimum) behind the in-service
 * 5-min heartbeat; it also restarts the service if the system killed it.
 */
class HeartbeatWorker(ctx: Context, params: WorkerParameters) : Worker(ctx, params) {
    companion object {
        private const val NAME = "heartbeat"
        fun schedule(ctx: Context) {
            val req = PeriodicWorkRequestBuilder<HeartbeatWorker>(15, TimeUnit.MINUTES)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
                .build()
            WorkManager.getInstance(ctx).enqueueUniquePeriodicWork(NAME, ExistingPeriodicWorkPolicy.KEEP, req)
        }
    }

    override fun doWork(): Result {
        if (!BridgeService.running) BridgeService.start(applicationContext)
        BridgeService.heartbeat(applicationContext)
        return Result.success()
    }
}

package app.kidpager.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat

/** A2: the foreground-service notification and the persistent "re-pair" notification (decision 2, 401). */
object Notifications {
    const val CH_SERVICE = "service"
    const val CH_ALERTS = "alerts"
    const val ID_SERVICE = 1
    const val ID_REPAIR = 2

    fun ensureChannels(ctx: Context) {
        val nm = ctx.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH_SERVICE, ctx.getString(R.string.channel_service), NotificationManager.IMPORTANCE_MIN))
        nm.createNotificationChannel(NotificationChannel(CH_ALERTS, ctx.getString(R.string.channel_alerts), NotificationManager.IMPORTANCE_DEFAULT))
    }

    private fun openApp(ctx: Context): PendingIntent = PendingIntent.getActivity(
        ctx, 0, Intent(ctx, SetupActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )

    fun service(ctx: Context, text: String): Notification = NotificationCompat.Builder(ctx, CH_SERVICE)
        .setSmallIcon(R.drawable.ic_bridge)
        .setContentTitle(ctx.getString(R.string.notif_running))
        .setContentText(text)
        .setOngoing(true)
        .setPriority(NotificationCompat.PRIORITY_MIN)
        .setContentIntent(openApp(ctx))
        .build()

    fun showRepair(ctx: Context) {
        ensureChannels(ctx)
        val n = NotificationCompat.Builder(ctx, CH_ALERTS)
            .setSmallIcon(R.drawable.ic_bridge)
            .setContentTitle(ctx.getString(R.string.notif_repair))
            .setContentText(ctx.getString(R.string.notif_repair_body))
            .setOngoing(true)
            .setContentIntent(openApp(ctx))
            .build()
        try { ctx.getSystemService(NotificationManager::class.java).notify(ID_REPAIR, n) } catch (e: SecurityException) { Log.w("notif", "cannot post: ${e.message}") }
    }

    fun clearRepair(ctx: Context) = ctx.getSystemService(NotificationManager::class.java).cancel(ID_REPAIR)
}

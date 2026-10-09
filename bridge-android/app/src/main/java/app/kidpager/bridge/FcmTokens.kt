package app.kidpager.bridge

import android.content.Context

/** A5: the FCM registration token sent in the heartbeat body; null in the poll-only build. */
object FcmTokens {
    fun current(ctx: Context): String? = if (BuildConfig.FCM) Prefs.fcmToken(ctx) else null
}

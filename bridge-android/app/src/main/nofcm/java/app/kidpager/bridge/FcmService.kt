package app.kidpager.bridge

import android.app.Service
import android.content.Intent
import android.os.IBinder

/**
 * A5 (poll-only build, BuildConfig.FCM = false): placeholder behind the manifest's
 * MESSAGING_EVENT filter so the same manifest serves both builds; never bound without Firebase.
 */
class FcmService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null
}

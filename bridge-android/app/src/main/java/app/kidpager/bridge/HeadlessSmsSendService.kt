package app.kidpager.bridge

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.telephony.TelephonyManager

/**
 * Decision 13 ("a quick-response service ... so the role is grantable"): the RESPOND_VIA_MESSAGE
 * handler the SMS role requires. The phone is headless, so it only logs and forwards the text.
 */
class HeadlessSmsSendService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == TelephonyManager.ACTION_RESPOND_VIA_MESSAGE) {
            val to = intent.data?.schemeSpecificPart
            val text = intent.getStringExtra(Intent.EXTRA_TEXT)
            if (to != null && !text.isNullOrBlank()) {
                SmsSender.send(this, "quick_${System.currentTimeMillis()}", to, text, null) { _, _ -> }
            }
        }
        stopSelf(startId)
        return START_NOT_STICKY
    }
}

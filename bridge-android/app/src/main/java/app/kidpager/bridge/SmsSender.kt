package app.kidpager.bridge

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.telephony.SmsManager

/**
 * Decision 13 ("SmsManager.getSmsManagerForSubscriptionId (dual SIM: `sim` from the outbox item,
 * default otherwise), divideMessage + sendMultipartTextMessage with sent and delivered
 * PendingIntents"). The outbox ack is `sent` when every part's sent intent returns RESULT_OK,
 * `failed sms_<code>` on the first error (A4), `sms_timeout` when the radio never answers.
 */
object SmsSender {
    private const val TAG = "smssend"
    const val ACTION_SENT = "app.kidpager.bridge.SMS_SENT"
    const val ACTION_DELIVERED = "app.kidpager.bridge.SMS_DELIVERED"
    private const val TIMEOUT_MS = 90_000L

    private class Pending(var partsLeft: Int, val onResult: (Boolean, String?) -> Unit)
    private val pending = HashMap<String, Pending>()
    private val main = Handler(Looper.getMainLooper())

    fun manager(ctx: Context, subscriptionId: Int?): SmsManager {
        val sub = subscriptionId ?: SmsManager.getDefaultSmsSubscriptionId()
        return if (Build.VERSION.SDK_INT >= 31) {
            val base = ctx.getSystemService(SmsManager::class.java)
            if (sub >= 0) base.createForSubscriptionId(sub) else base
        } else {
            @Suppress("DEPRECATION")
            if (sub >= 0) SmsManager.getSmsManagerForSubscriptionId(sub) else SmsManager.getDefault()
        }
    }

    /** Sends `text` to `to`; `onResult(ok, reason)` fires exactly once, off the caller's thread. */
    fun send(ctx: Context, sendId: String, to: String, text: String, subscriptionId: Int?, onResult: (Boolean, String?) -> Unit) {
        val sm = manager(ctx, subscriptionId)
        val parts = sm.divideMessage(text)
        if (parts.isEmpty()) { onResult(false, "sms_empty"); return }
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
        val sent = ArrayList<PendingIntent>()
        val delivered = ArrayList<PendingIntent>()
        for (i in parts.indices) {
            val s = Intent(ctx, SmsResultReceiver::class.java).setAction(ACTION_SENT)
                .putExtra("id", sendId).putExtra("part", i)
            val d = Intent(ctx, SmsResultReceiver::class.java).setAction(ACTION_DELIVERED)
                .putExtra("id", sendId).putExtra("part", i)
            sent += PendingIntent.getBroadcast(ctx, ("$sendId/s/$i").hashCode(), s, flags)
            delivered += PendingIntent.getBroadcast(ctx, ("$sendId/d/$i").hashCode(), d, flags)
        }
        synchronized(pending) { pending[sendId] = Pending(parts.size, onResult) }
        main.postDelayed({ finish(sendId, false, "sms_timeout") }, TIMEOUT_MS)
        try {
            sm.sendMultipartTextMessage(to, null, parts, sent, delivered)
            Log.i(TAG, "out id=$sendId to=${Log.redact(to)} parts=${parts.size} sub=${subscriptionId ?: "default"}")
        } catch (e: Exception) {
            Log.e(TAG, "send threw", e)
            finish(sendId, false, "sms_exception")
        }
    }

    internal fun onPartSent(sendId: String, resultCode: Int) {
        if (resultCode == Activity.RESULT_OK) {
            val done = synchronized(pending) {
                val p = pending[sendId] ?: return
                p.partsLeft -= 1
                p.partsLeft <= 0
            }
            if (done) finish(sendId, true, null)
        } else {
            finish(sendId, false, "sms_$resultCode")
        }
    }

    private fun finish(sendId: String, ok: Boolean, reason: String?) {
        val p = synchronized(pending) { pending.remove(sendId) } ?: return
        Log.i(TAG, "result id=$sendId ok=$ok reason=${reason ?: "-"}")
        p.onResult(ok, reason)
    }
}

/** Receives the sent/delivered PendingIntents of [SmsSender]. */
class SmsResultReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val id = intent.getStringExtra("id") ?: return
        when (intent.action) {
            SmsSender.ACTION_SENT -> SmsSender.onPartSent(id, resultCode)
            SmsSender.ACTION_DELIVERED -> Log.i("smssend", "delivered id=$id part=${intent.getIntExtra("part", -1)}")
        }
    }
}

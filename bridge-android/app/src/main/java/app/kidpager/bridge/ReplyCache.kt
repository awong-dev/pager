package app.kidpager.bridge

import android.app.Notification
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat

/**
 * Decision 13: "reply-action cache conversationId -> (action, RemoteInput[]) from
 * notification.actions and NotificationCompat.WearableExtender(notification).actions, rebuilt
 * from getActiveNotifications() on onListenerConnected" and the tier-1 reply:
 * RemoteInput.addResultsToIntent + actionIntent.send; CanceledException -> tier 2.
 * The phone -> conversation map is keyed `source|phone` and is filled for Google Voice only
 * (L8, 10 Oct 2026): a `gvoice` send may carry just `to.phone`, while a WhatsApp send always
 * carries the conversation id (the JID), so WhatsApp no longer uses it.
 */
object ReplyCache {
    private const val TAG = "reply"

    /** One cached reply action; `fire` fills the RemoteInput(s) and sends the PendingIntent. */
    class Entry(val pkg: String, val label: String, private val fire: (Context, String) -> Unit) {
        fun send(ctx: Context, text: String) = fire(ctx, text)
    }

    private val byConversation = HashMap<String, Entry>()
    /** `source|phone` -> conversation id, so a `gvoice` send with only `to.phone` finds its thread (Voice only). */
    private val byPhone = HashMap<String, String>()

    @Synchronized fun size() = byConversation.size

    @Synchronized
    fun clear() { byConversation.clear(); byPhone.clear() }

    @Synchronized
    fun rememberPhone(source: String, phone: String, conversationId: String) { byPhone["$source|$phone"] = conversationId }

    @Synchronized
    fun conversationForPhone(source: String, phone: String): String? = byPhone["$source|$phone"]

    @Synchronized
    fun has(conversationId: String) = byConversation.containsKey(conversationId)

    /** Records the notification's reply action under `conversationId`; true when one was found. */
    @Synchronized
    fun remember(conversationId: String, sbn: StatusBarNotification): Boolean {
        val entry = extract(sbn) ?: return false
        byConversation[conversationId] = entry
        return true
    }

    private fun extract(sbn: StatusBarNotification): Entry? {
        val n = sbn.notification
        n.actions?.forEach { a ->
            val ris = a.remoteInputs
            if (!ris.isNullOrEmpty() && a.actionIntent != null) {
                return Entry(sbn.packageName, a.title?.toString() ?: "reply") { ctx, text ->
                    val intent = Intent()
                    val results = Bundle()
                    for (ri in ris) results.putCharSequence(ri.resultKey, text)
                    android.app.RemoteInput.addResultsToIntent(ris, intent, results)
                    a.actionIntent.send(ctx, 0, intent)
                }
            }
        }
        NotificationCompat.WearableExtender(n).actions.forEach { a ->
            val ris = a.remoteInputs
            if (!ris.isNullOrEmpty() && a.actionIntent != null) {
                return Entry(sbn.packageName, a.title?.toString() ?: "reply") { ctx, text ->
                    val intent = Intent()
                    val results = Bundle()
                    for (ri in ris) results.putCharSequence(ri.resultKey, text)
                    androidx.core.app.RemoteInput.addResultsToIntent(ris, intent, results)
                    a.actionIntent!!.send(ctx, 0, intent)
                }
            }
        }
        return null
    }

    /**
     * Tier 1. True when the PendingIntent was sent; false when there is no cached action or
     * the app cancelled it (reboot, update, notification dismissed by the app).
     */
    fun reply(ctx: Context, conversationId: String, text: String): Boolean {
        val entry = synchronized(this) { byConversation[conversationId] } ?: run {
            Log.i(TAG, "no cached action for $conversationId")
            return false
        }
        return try {
            entry.send(ctx, text)
            Log.i(TAG, "tier1 sent conv=$conversationId via ${entry.pkg}")
            true
        } catch (e: PendingIntent.CanceledException) {
            Log.w(TAG, "tier1 cancelled conv=$conversationId; dropping action")
            synchronized(this) { byConversation.remove(conversationId) }
            false
        }
    }

    /** Debug view for the log screen. */
    @Synchronized
    fun describe(): String = byConversation.entries.joinToString("\n") { "${it.key} -> ${it.value.pkg} [${it.value.label}]" }
}

package app.kidpager.bridge

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Decision 13 (listener bullet): reads MessagingStyle notifications from Google Chat and Google
 * Voice, maps them to `BridgeEvent`s (NotificationMapper), dedups through Room, caches the reply
 * action (ReplyCache) and spools events (EventQueue). Rebuilds the cache from
 * getActiveNotifications() on connect.
 */
class ChatNotificationListener : NotificationListenerService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    companion object {
        private const val TAG = "listener"
        @Volatile var connected = false
            private set
    }

    override fun onListenerConnected() {
        connected = true
        Log.i(TAG, "connected")
        scope.launch {
            AppDb.get(this@ChatNotificationListener).seen().purgeBefore(System.currentTimeMillis() - Dedup.TTL_MS)
            rebuild()
        }
    }

    override fun onListenerDisconnected() {
        connected = false
        Log.w(TAG, "disconnected")
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName !in Targets.LISTEN_PACKAGES) return
        scope.launch { handle(sbn, emitEvents = true) }
    }

    /** Decision 13: the cache is rebuilt from what is on the shade; messages already seen are dedup'd. */
    fun rebuild() {
        val active = try { activeNotifications } catch (e: Exception) { Log.w(TAG, "activeNotifications: ${e.message}"); return }
        ReplyCache.clear()
        var n = 0
        for (sbn in active ?: emptyArray()) {
            if (sbn.packageName !in Targets.LISTEN_PACKAGES) continue
            handle(sbn, emitEvents = true)
            n++
        }
        Log.i(TAG, "rebuilt from ${n} active notifications; ${ReplyCache.size()} reply actions")
    }

    private fun handle(sbn: StatusBarNotification, emitEvents: Boolean) {
        val snapshot = snapshot(sbn) ?: return
        val convId = NotificationMapper.conversationId(snapshot)
        val hadAction = ReplyCache.remember(convId, sbn)
        if (!emitEvents) return
        val db = AppDb.get(this)
        val mapped = NotificationMapper.map(snapshot, seen = { db.seen().count(it) > 0 })
        if (mapped.voicePeerPhone != null) ReplyCache.rememberVoicePhone(mapped.voicePeerPhone, convId)
        if (mapped.events.isEmpty()) return
        EventQueue.enqueue(this, mapped.events)
        val now = System.currentTimeMillis()
        for (k in mapped.dedupKeys) db.seen().insert(SeenMessage(k, now))
        Log.i(TAG, "${sbn.packageName.substringAfterLast('.')} conv=$convId +${mapped.events.size} action=${hadAction}")
    }

    /** Flattens the Android objects so the mapper stays pure. */
    private fun snapshot(sbn: StatusBarNotification): NotificationSnapshot? {
        val n = sbn.notification
        val style = NotificationCompat.MessagingStyle.extractMessagingStyleFromNotification(n) ?: return null
        val self = style.user.name?.toString()
        val messages = style.messages.map { m ->
            val person = m.person
            SnapshotMessage(
                senderName = person?.name?.toString(),
                text = m.text?.toString(),
                timestamp = m.timestamp,
                isSelf = person == null,
            )
        }
        val shortcut = if (android.os.Build.VERSION.SDK_INT >= 26) n.shortcutId else null
        return NotificationSnapshot(
            pkg = sbn.packageName,
            key = sbn.key,
            id = sbn.id,
            shortcutId = shortcut,
            dataUri = n.extras?.getString("android.intent.extra.TEXT")?.takeIf { it.startsWith("tel:") }
                ?: n.extras?.getCharSequence(android.app.Notification.EXTRA_SUB_TEXT)?.toString(),
            conversationTitle = style.conversationTitle?.toString(),
            isGroup = style.isGroupConversation,
            selfName = self,
            messages = messages,
        )
    }
}

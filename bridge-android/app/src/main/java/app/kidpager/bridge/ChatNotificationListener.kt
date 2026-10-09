package app.kidpager.bridge

import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Decision 13 (listener bullet): reads MessagingStyle notifications from Google Chat, Google
 * Voice and WhatsApp (WA6), maps them to `BridgeEvent`s (NotificationMapper), dedups through
 * Room, caches the reply action (ReplyCache) and spools events (EventQueue). Rebuilds the cache
 * from getActiveNotifications() on connect. Non-MessagingStyle notifications (calls, "checking
 * for new messages", backups, status) and group summaries (`FLAG_GROUP_SUMMARY`) are ignored.
 */
class ChatNotificationListener : NotificationListenerService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    companion object {
        private const val TAG = "listener"
        @Volatile var connected = false
            private set
        @Volatile private var lidLogged = false
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
        if (!wanted(sbn)) return
        scope.launch { handle(sbn, emitEvents = true) }
    }

    /** Listened package and not a "N messages from M chats" group summary (WA6). */
    private fun wanted(sbn: StatusBarNotification): Boolean =
        sbn.packageName in Targets.LISTEN_PACKAGES && (sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY) == 0

    /** Decision 13: the cache is rebuilt from what is on the shade; messages already seen are dedup'd. */
    fun rebuild() {
        val active = try { activeNotifications } catch (e: Exception) { Log.w(TAG, "activeNotifications: ${e.message}"); return }
        ReplyCache.clear()
        var n = 0
        for (sbn in active ?: emptyArray()) {
            if (!wanted(sbn)) continue
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
        val source = Targets.sourceFor(snapshot.pkg) ?: return
        if (source == Targets.SOURCE_WHATSAPP && !snapshot.isGroup && snapshot.shortcutId?.let { Targets.WA_LID_JID.matches(it) } == true && !lidLogged) {
            lidLogged = true
            Log.w(TAG, "whatsapp DM with a LID jid (no number); the relay will drop it unless the sender line is a number")
        }
        val db = AppDb.get(this)
        val mapped = NotificationMapper.map(snapshot, seen = { db.seen().count(it) > 0 })
        if (mapped.peerPhone != null) ReplyCache.rememberPhone(source, mapped.peerPhone, convId)
        if (mapped.events.isEmpty()) return
        EventQueue.enqueue(this, mapped.events)
        val now = System.currentTimeMillis()
        for (k in mapped.dedupKeys) db.seen().insert(SeenMessage(k, now))
        Log.i(TAG, "${sbn.packageName.substringAfterLast('.')} conv=$convId +${mapped.events.size} action=${hadAction}")
    }

    /** Flattens the Android objects so the mapper stays pure; null for anything that is not MessagingStyle. */
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

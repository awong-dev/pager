package app.kidpager.bridge

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.provider.Telephony
import android.telephony.SmsManager
import java.io.File

/**
 * Decision 13 ("WAP_PUSH_DELIVER receiver, BROADCAST_WAP_PUSH permission; MMS text extracted
 * from the PDU, attachments reported by kind"): the M-Notification.ind arrives here, the body is
 * fetched with SmsManager.downloadMultimediaMessage into [MmsFileProvider], parsed by [MmsPdu]
 * and sent as one `sms` event (text part + attachment kinds; empty text becomes `[photo]` on
 * the relay, decision 5). onReceive runs on the main thread and must stay off Room: the spool
 * insert goes through goAsync() + EventQueue.enqueueAsync (A8 review fix); [event] is the pure
 * PDU -> event step.
 */
class MmsReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "mms"
        const val ACTION_DOWNLOADED = "app.kidpager.bridge.MMS_DOWNLOADED"

        /** Pure: a parsed M-Retrieve.conf plus the sender -> one `sms` event. */
        fun event(from: String, pdu: MmsPdu.Pdu, tsMillis: Long): BridgeEvent {
            val phone = PhoneNumbers.normalize(from) ?: from
            val text = pdu.text
            val attachments = pdu.attachments
            return BridgeEvent(
                id = NotificationMapper.eventId("mms:$phone", tsMillis, text + attachments.size).replaceFirst("n_", "m_"),
                source = Targets.SOURCE_SMS,
                conversation = Conversation(id = phone, isGroup = false),
                sender = Sender(name = phone, phone = phone),
                text = Bounds.cp(text, Bounds.TEXT_CP),
                ts = tsMillis / 1000,
                attachments = attachments.ifEmpty { null },
            )
        }
    }

    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Telephony.Sms.Intents.WAP_PUSH_DELIVER_ACTION -> onNotification(context, intent)
            ACTION_DOWNLOADED -> onDownloaded(context, intent)
        }
    }

    private fun onNotification(context: Context, intent: Intent) {
        val data = intent.getByteArrayExtra("data") ?: return
        val pdu = try { MmsPdu.parse(data) } catch (e: Exception) { Log.e(TAG, "bad notification pdu", e); return }
        if (pdu.messageType != MmsPdu.MESSAGE_TYPE_NOTIFICATION_IND || pdu.contentLocation == null) {
            Log.w(TAG, "ignored pdu type=${pdu.messageType}")
            return
        }
        val file = "mms_${System.currentTimeMillis()}_${(pdu.transactionId ?: "").hashCode().toUInt()}.pdu"
        val uri = MmsFileProvider.uriFor(context, file)
        val done = Intent(context, MmsReceiver::class.java).setAction(ACTION_DOWNLOADED)
            .putExtra("file", file).putExtra("from", pdu.from)
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
        val pi = PendingIntent.getBroadcast(context, file.hashCode(), done, flags)
        val subId = intent.getIntExtra(
            "android.telephony.extra.SUBSCRIPTION_INDEX",
            intent.getIntExtra("subscription", SmsManager.getDefaultSmsSubscriptionId()),
        )
        Log.i(TAG, "notification from=${Log.redact(pdu.from)} downloading")
        SmsSender.manager(context, subId).downloadMultimediaMessage(context, pdu.contentLocation, uri, null, pi)
    }

    private fun onDownloaded(context: Context, intent: Intent) {
        val file = intent.getStringExtra("file") ?: return
        val f = File(MmsFileProvider.dir(context), file)
        if (resultCode != Activity.RESULT_OK) {
            Log.w(TAG, "download failed code=$resultCode")
            f.delete()
            return
        }
        val pdu = try { MmsPdu.parse(f.readBytes()) } catch (e: Exception) { Log.e(TAG, "bad retrieve pdu", e); f.delete(); return }
        f.delete()
        val from = pdu.from ?: intent.getStringExtra("from") ?: run { Log.w(TAG, "no sender"); return }
        val e = event(from, pdu, System.currentTimeMillis())
        Log.i(TAG, "in from=${Log.redact(e.sender.phone)} len=${e.text.length} attachments=${e.attachments?.map { it.kind } ?: emptyList<String>()}")
        val result = goAsync()
        EventQueue.enqueueAsync(context, listOf(e)) { result.finish() }
    }
}

/** File-backed content URIs for the platform MMS service to write the downloaded PDU into. */
class MmsFileProvider : ContentProvider() {
    companion object {
        const val AUTHORITY = "app.kidpager.bridge.mms"
        fun dir(ctx: Context): File = File(ctx.cacheDir, "mms").apply { mkdirs() }
        fun uriFor(ctx: Context, name: String): Uri {
            dir(ctx)
            return Uri.parse("content://$AUTHORITY/$name")
        }
    }

    override fun onCreate() = true
    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val ctx = context ?: return null
        val f = File(dir(ctx), uri.lastPathSegment ?: return null)
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.parseMode(mode))
    }
    override fun query(uri: Uri, p: Array<String>?, s: String?, a: Array<String>?, o: String?): Cursor? = null
    override fun getType(uri: Uri): String = "application/vnd.wap.mms-message"
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, s: String?, a: Array<String>?) = 0
    override fun update(uri: Uri, v: ContentValues?, s: String?, a: Array<String>?) = 0
}

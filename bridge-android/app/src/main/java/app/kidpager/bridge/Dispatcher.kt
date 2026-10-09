package app.kidpager.bridge

import android.content.Context
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * A5 tier dispatch: `send` with `to.phone` on source `sms` -> SmsSender (A4); `send` on
 * `gchat` -> ReplyCache tier 1, else tier 2 with `to.link`; `send` on `gvoice` (O1 revised) ->
 * cached Voice reply action for `to.conversationId` or the peer number, else tier 2 opens
 * `to.link` (or the Voice thread URL for the number); `inspect` -> tier 2 inspect + an
 * `inspect` event. Failure reasons: `no_link`, `ui_changed`, `no_accessibility`, `sms_<code>`.
 */
object Dispatcher {
    private const val TAG = "dispatch"

    data class Outcome(val state: String, val reason: String?, val tier: Int)

    suspend fun dispatch(ctx: Context, item: RelayClient.OutboxItem): Outcome {
        val text = item.text.orEmpty()
        val to = item.to
        return when (item.kind) {
            "inspect" -> inspect(ctx, item.link ?: to?.link)
            "send" -> when {
                item.source == Targets.SOURCE_SMS && to?.phone != null -> sms(ctx, item.id, to.phone, text, item.sim)
                item.source == Targets.SOURCE_GVOICE -> voice(ctx, to, text)
                to?.conversationId != null || to?.link != null -> chat(ctx, to.conversationId, to.link, text)
                to?.phone != null -> sms(ctx, item.id, to.phone, text, item.sim)
                else -> Outcome("failed", "no_link", 1)
            }
            else -> Outcome("failed", "unsupported", 1)
        }
    }

    private suspend fun sms(ctx: Context, id: String, phone: String, text: String, sim: Int?): Outcome {
        if (!Status.smsDefault(ctx)) return Outcome("failed", "sms_not_default", 1)
        if (text.isBlank()) return Outcome("failed", "sms_empty", 1)
        val (ok, reason) = suspendCancellableCoroutine { cont ->
            SmsSender.send(ctx, id, phone, text, sim) { ok, reason -> if (cont.isActive) cont.resume(ok to reason) }
        }
        return if (ok) Outcome("sent", null, 1) else Outcome("failed", reason, 1)
    }

    private suspend fun chat(ctx: Context, conversationId: String?, link: String?, text: String): Outcome {
        if (conversationId != null && ReplyCache.reply(ctx, conversationId, text)) return Outcome("sent", null, 1)
        return tier2Send(link, text)
    }

    private suspend fun voice(ctx: Context, to: RelayClient.OutboxTo?, text: String): Outcome {
        val phone = to?.phone?.let { PhoneNumbers.normalize(it) }
        val conv = to?.conversationId ?: phone?.let { ReplyCache.conversationForPhone(it) }
        if (conv != null && ReplyCache.reply(ctx, conv, text)) return Outcome("sent", null, 1)
        val link = to?.link ?: phone?.let { Targets.voiceThreadLink(it) }
        return tier2Send(link, text)
    }

    private suspend fun tier2Send(link: String?, text: String): Outcome {
        if (link.isNullOrBlank()) return Outcome("failed", "no_link", 1)
        val svc = BridgeAccessibilityService.instance ?: return Outcome("failed", "no_accessibility", 2)
        val reason = svc.sendViaUi(link, text)
        return if (reason == null) Outcome("sent", null, 2) else Outcome("failed", reason, 2)
    }

    private suspend fun inspect(ctx: Context, link: String?): Outcome {
        if (link.isNullOrBlank()) return Outcome("failed", "no_link", 2)
        val svc = BridgeAccessibilityService.instance ?: return Outcome("failed", "no_accessibility", 2)
        val r = svc.inspect(link) ?: return Outcome("failed", "ui_changed", 2)
        val source = if (Targets.packageForLink(link) == Targets.GVOICE_PKG) Targets.SOURCE_GVOICE else Targets.SOURCE_GCHAT
        val ev = BridgeEvent(
            id = "i_" + NotificationMapper.eventId(r.conversationId, System.currentTimeMillis(), link).removePrefix("n_"),
            source = source,
            kind = "inspect",
            conversation = Conversation(id = Bounds.convId(r.conversationId), title = r.title?.let { Bounds.cp(it, Bounds.NAME_CP) }, isGroup = r.isGroup, link = link),
            sender = Sender(name = "bridge"),
            text = "",
            ts = System.currentTimeMillis() / 1000,
            people = r.people.take(Bounds.PEOPLE).ifEmpty { null },
        )
        EventQueue.enqueue(ctx, listOf(ev))
        Log.i(TAG, "inspect reported conv=${r.conversationId}")
        return Outcome("sent", null, 2)
    }
}

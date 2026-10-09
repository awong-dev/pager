package app.kidpager.bridge

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/**
 * Decision 5: the `POST /bridge/events` wire shape, exactly as docs/BRIDGE_PHONE_DESIGN.md
 * lists it. `ts` is Unix epoch seconds like every other relay timestamp (PROTOCOL.md §3.1).
 */
@Serializable
data class Conversation(
    val id: String,
    val title: String? = null,
    val isGroup: Boolean = false,
    val link: String? = null,
)

@Serializable
data class Sender(val name: String, val phone: String? = null)

@Serializable
data class Attachment(val kind: String)

@Serializable
data class BridgeEvent(
    val id: String,
    val source: String,
    val kind: String? = null,
    val conversation: Conversation,
    val sender: Sender,
    val text: String,
    val ts: Long,
    val attachments: List<Attachment>? = null,
    val people: List<String>? = null,
)

@Serializable
data class EventsRequest(val events: List<BridgeEvent>)

@Serializable
data class EventResult(val id: String, val outcome: String)

@Serializable
data class EventsResponse(val results: List<EventResult> = emptyList())

val WireJson = Json {
    ignoreUnknownKeys = true
    explicitNulls = false
    encodeDefaults = true // isGroup:false must be on the wire (decision 5); nulls are still omitted
}

/** Decision 5 bounds (422 on violation): text <=1600 cp, title/sender.name <=200 cp, conversation.id <=512 B. */
object Bounds {
    const val TEXT_CP = 1600
    const val NAME_CP = 200
    const val CONV_ID_BYTES = 512
    const val BATCH = 50
    const val PEOPLE = 64
    fun cp(s: String, max: Int): String = if (s.codePointCount(0, s.length) <= max) s else s.substring(0, s.offsetByCodePoints(0, max))
    fun convId(s: String): String {
        var t = s
        while (t.toByteArray().size > CONV_ID_BYTES) t = t.dropLast(1)
        return t
    }
}

/** Attachment kind from a MIME type (decision 5 `attachments[].kind`). */
fun attachmentKind(mime: String?): String = when {
    mime == null -> "file"
    mime.startsWith("image/") -> "image"
    mime.startsWith("video/") -> "video"
    mime.startsWith("audio/") -> "audio"
    else -> "file"
}

package app.kidpager.bridge

import java.nio.charset.Charset

/**
 * Decision 13 ("MMS text extracted from the PDU, attachments reported by kind"): a minimal
 * WSP/MMS (OMA-MMS-ENC) parser for the two PDUs the default SMS app sees -- the
 * M-Notification.ind that WAP_PUSH_DELIVER hands over (content location + sender) and the
 * M-Retrieve.conf that SmsManager.downloadMultimediaMessage writes (headers + multipart body).
 * Pure Kotlin, unit-tested with hand-built PDUs.
 */
object MmsPdu {
    const val MESSAGE_TYPE_NOTIFICATION_IND = 0x82
    const val MESSAGE_TYPE_RETRIEVE_CONF = 0x84

    private const val H_CONTENT_LOCATION = 0x83
    private const val H_CONTENT_TYPE = 0x84
    private const val H_FROM = 0x89
    private const val H_MESSAGE_TYPE = 0x8C
    private const val H_TRANSACTION_ID = 0x98

    data class Part(val contentType: String, val data: ByteArray, val name: String?)

    data class Pdu(
        val messageType: Int,
        val from: String?,
        val transactionId: String?,
        val contentLocation: String?,
        val contentType: String?,
        val parts: List<Part>,
    ) {
        /** Concatenated text/plain parts (SMIL is layout, never text). */
        val text: String
            get() = parts.filter { it.contentType.startsWith("text/plain") }
                .joinToString("\n") { String(it.data, charsetOf(it.contentType)).trim() }
                .trim()

        /** Decision 5 attachment kinds for every non-text, non-SMIL part. */
        val attachments: List<Attachment>
            get() = parts.filter { !it.contentType.startsWith("text/plain") && !it.contentType.startsWith("application/smil") }
                .map { Attachment(attachmentKind(it.contentType)) }
    }

    /** WSP well-known content types (Appendix A of WAP-230-WSP) that MMS bodies actually use. */
    private val WELL_KNOWN = mapOf(
        0x00 to "*/*", 0x01 to "text/*", 0x02 to "text/html", 0x03 to "text/plain",
        0x1D to "image/gif", 0x1E to "image/jpeg", 0x1F to "image/tiff", 0x20 to "image/png",
        0x21 to "image/vnd.wap.wbmp", 0x23 to "application/vnd.wap.multipart.mixed",
        0x26 to "application/vnd.wap.multipart.alternative", 0x33 to "application/vnd.wap.multipart.related",
        0x3E to "image/bmp", 0x37 to "image/*", 0x3F to "application/vnd.wap.mms-message",
    )

    private class Reader(val b: ByteArray, var pos: Int = 0) {
        fun u8(): Int = b[pos++].toInt() and 0xFF
        fun peek(): Int = b[pos].toInt() and 0xFF
        val remaining get() = b.size - pos
        fun uintvar(): Int {
            var v = 0
            while (true) {
                val x = u8()
                v = (v shl 7) or (x and 0x7F)
                if (x and 0x80 == 0) return v
            }
        }
        fun textString(): String {
            if (peek() == 0x7F) pos++ // quote byte
            val start = pos
            while (pos < b.size && b[pos].toInt() != 0) pos++
            val s = String(b, start, pos - start, Charsets.UTF_8)
            if (pos < b.size) pos++ // NUL
            return s
        }
        /** Value-length (WSP 8.4.2.2): 0..30 literal, 31 -> uintvar. */
        fun valueLength(): Int {
            val x = u8()
            return if (x <= 30) x else uintvar()
        }
        /** Skips (or returns the span of) one WSP header value, sniffing its form from the first byte. */
        fun skipValue() {
            val x = peek()
            when {
                x <= 31 -> { val len = valueLength(); pos += len }
                x < 128 -> textString()
                else -> pos++
            }
        }
        fun longInteger(): Long {
            val len = u8()
            var v = 0L
            repeat(len) { v = (v shl 8) or u8().toLong() }
            return v
        }
    }

    /** Content-type-value: constrained (short int or text) or general form (length + media + params). */
    private fun contentType(r: Reader): Pair<String, Map<String, String>> {
        val params = HashMap<String, String>()
        val x = r.peek()
        if (x > 31 && x < 128) return r.textString() to params
        if (x >= 128) { r.pos++; return (WELL_KNOWN[x and 0x7F] ?: "application/octet-stream") to params }
        val len = r.valueLength()
        val end = r.pos + len
        val media = contentType(r).first
        while (r.pos < end) {
            val p = r.peek()
            if (p >= 128) {
                r.pos++
                val code = p and 0x7F
                val v = r.peek()
                val value = when {
                    v >= 128 -> { r.pos++; (v and 0x7F).toString() }
                    v > 31 -> r.textString()
                    else -> { val l = r.valueLength(); r.pos += l; "" }
                }
                when (code) {
                    0x01 -> params["charset"] = charsetName(value) // well-known charset code
                    0x05, 0x17 -> params["name"] = value
                    0x09, 0x18 -> params["type"] = value
                    0x0E -> params["start"] = value
                }
            } else {
                val name = r.textString()
                val value = if (r.peek() >= 128) { r.pos++; "" } else r.textString()
                params[name.lowercase()] = value
            }
        }
        r.pos = end
        return media to params
    }

    private fun charsetName(code: String): String = when (code) {
        "3" -> "us-ascii"; "4" -> "iso-8859-1"; "106" -> "utf-8"; "1015" -> "utf-16"; else -> "utf-8"
    }

    private fun charsetOf(contentType: String): Charset {
        val cs = Regex("charset=([^;\\s]+)", RegexOption.IGNORE_CASE).find(contentType)?.groupValues?.get(1)
        return try { if (cs != null) Charset.forName(cs.trim('"')) else Charsets.UTF_8 } catch (e: Exception) { Charsets.UTF_8 }
    }

    /** From: value-length, then Address-present-token (0x80) + encoded-string or Insert-address-token (0x81). */
    private fun fromAddress(r: Reader): String? {
        val len = r.valueLength()
        val end = r.pos + len
        var out: String? = null
        if (len > 0) {
            val tok = r.u8()
            if (tok == 0x80 && r.pos < end) {
                out = encodedString(r)
            }
        }
        r.pos = end
        return out?.substringBefore("/TYPE=")?.ifBlank { null }
    }

    /** Encoded-string-value: text-string, or value-length + charset(short int) + text-string. */
    private fun encodedString(r: Reader): String {
        val x = r.peek()
        if (x > 31) return r.textString()
        val len = r.valueLength()
        val end = r.pos + len
        var cs = Charsets.UTF_8
        if (r.peek() >= 128) cs = try { Charset.forName(charsetName((r.u8() and 0x7F).toString())) } catch (e: Exception) { Charsets.UTF_8 }
        val start = r.pos
        var e = start
        while (e < end && r.b[e].toInt() != 0) e++
        val s = String(r.b, start, e - start, cs)
        r.pos = end
        return s
    }

    fun parse(bytes: ByteArray): Pdu {
        val r = Reader(bytes)
        var messageType = 0
        var from: String? = null
        var txn: String? = null
        var location: String? = null
        var contentType: String? = null
        var ctParams: Map<String, String> = emptyMap()
        while (r.remaining > 0) {
            val h = r.peek()
            if (h < 128) { // text header: name NUL value NUL
                r.textString(); r.textString(); continue
            }
            r.pos++
            when (h) {
                H_MESSAGE_TYPE -> messageType = r.u8() // wire value, 0x82 / 0x84
                H_FROM -> from = fromAddress(r)
                H_TRANSACTION_ID -> txn = r.textString()
                H_CONTENT_LOCATION -> location = r.textString()
                H_CONTENT_TYPE -> { val (m, p) = contentType(r); contentType = m; ctParams = p }
                else -> r.skipValue()
            }
            if (h == H_CONTENT_TYPE) break // Content-Type is the last header; the body follows
        }
        val parts = ArrayList<Part>()
        if (contentType != null && r.remaining > 0) {
            if (contentType.startsWith("application/vnd.wap.multipart")) {
                val n = r.uintvar()
                repeat(n) {
                    if (r.remaining <= 0) return@repeat
                    val headersLen = r.uintvar()
                    val dataLen = r.uintvar()
                    val hStart = r.pos
                    val (media, p) = contentType(r)
                    r.pos = hStart + headersLen
                    val data = bytes.copyOfRange(r.pos, (r.pos + dataLen).coerceAtMost(bytes.size))
                    r.pos += dataLen
                    val ct = if (p["charset"] != null) "$media; charset=${p["charset"]}" else media
                    parts += Part(ct, data, p["name"])
                }
            } else {
                val ct = if (ctParams["charset"] != null) "$contentType; charset=${ctParams["charset"]}" else contentType
                parts += Part(ct, bytes.copyOfRange(r.pos, bytes.size), null)
            }
        }
        return Pdu(messageType, from, txn, location, contentType, parts)
    }
}

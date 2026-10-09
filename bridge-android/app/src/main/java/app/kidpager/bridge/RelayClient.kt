package app.kidpager.bridge

import android.content.Context
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * Decisions 2, 5 and 11: the `/bridge/...` HTTP contract. `pair` is unauthenticated; every
 * other call carries `Authorization: Bearer <bridgeId>.<secret>`. Retries with backoff on
 * 5xx/IO; a 401 clears the token (decision 2: "a missing/invalid token is a uniform 401")
 * and the service shows a re-pair notification.
 */
class RelayClient(private val ctx: Context) {
    class HttpError(val code: Int, val body: String) : IOException("HTTP $code: ${body.take(200)}")
    class Unauthorized : IOException("401 unauthorized")

    companion object {
        private const val TAG = "relay"
        private val JSON = "application/json; charset=utf-8".toMediaType()
        @Volatile var onUnauthorized: ((Context) -> Unit)? = null
    }

    private val http = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(40, TimeUnit.SECONDS)
        .writeTimeout(20, TimeUnit.SECONDS)
        .callTimeout(60, TimeUnit.SECONDS)
        .build()

    private fun base(): String = Prefs.relayUrl(ctx)

    // ---- decision 2: POST /bridge/pair {code, version, accounts, simNumber?, voiceNumber?, caps} ----
    @Serializable
    data class Caps(val sms: Boolean, val gchat: Boolean, val gvoice: Boolean)

    @Serializable
    data class PairRequest(
        val code: String,
        val version: String,
        val accounts: List<String>,
        val simNumber: String? = null,
        val voiceNumber: String? = null,
        val caps: Caps,
    )

    @Serializable
    data class PairResponse(val bridgeId: String, val token: String)

    fun pair(req: PairRequest): PairResponse {
        val body = post("/bridge/pair", WireJson.encodeToString(req), auth = false, retries = 0)
        return WireJson.decodeFromString(PairResponse.serializer(), body)
    }

    // ---- decision 5: POST /bridge/events ----
    fun events(events: List<BridgeEvent>): EventsResponse {
        val body = post("/bridge/events", WireJson.encodeToString(EventsRequest(events)))
        return WireJson.decodeFromString(EventsResponse.serializer(), body)
    }

    // ---- decision 11: GET /bridge/outbox?wait=25 ----
    @Serializable
    data class OutboxTo(val phone: String? = null, val conversationId: String? = null, val link: String? = null)

    @Serializable
    data class OutboxItem(
        val id: String,
        val kind: String,
        val source: String? = null,
        val to: OutboxTo? = null,
        val text: String? = null,
        val link: String? = null,
        val msgId: String? = null,
        val bid: String? = null,
        val replyHint: String? = null,
        val sim: Int? = null,
    )

    @Serializable
    data class OutboxResponse(val items: List<OutboxItem> = emptyList())

    /** O2: the phone never long-polls; `wait` stays 0 (the parameter exists for the simulator). */
    fun outbox(wait: Int = 0): OutboxResponse {
        val body = get("/bridge/outbox?wait=$wait")
        return WireJson.decodeFromString(OutboxResponse.serializer(), body)
    }

    // ---- decision 11: POST /bridge/outbox/{obId}/ack {state, reason?, tier} ----
    @Serializable
    data class AckRequest(val state: String, val reason: String? = null, val tier: Int)

    fun ack(obId: String, state: String, reason: String?, tier: Int) {
        post("/bridge/outbox/$obId/ack", WireJson.encodeToString(AckRequest(state, reason, tier)))
    }

    // ---- decision 11 + O2: POST /bridge/heartbeat {status, fcmToken?} -> {pending: n} ----
    @Serializable
    data class HeartbeatResponse(val pending: Int = 0)

    /** Returns the relay's count of pending outbox items (0 when the body is empty, e.g. a 204). */
    fun heartbeat(status: JsonObject, fcmToken: String?): Int {
        val body = buildJsonObject {
            put("status", status)
            if (fcmToken != null) put("fcmToken", JsonPrimitive(fcmToken))
        }
        val text = post("/bridge/heartbeat", WireJson.encodeToString(JsonElement.serializer(), body))
        if (text.isBlank()) return 0
        return try { WireJson.decodeFromString(HeartbeatResponse.serializer(), text).pending } catch (e: Exception) { 0 }
    }

    // ---- transport ----
    private fun get(path: String): String = exec(Request.Builder().url(base() + path).get(), auth = true, retries = 2)

    private fun post(path: String, json: String, auth: Boolean = true, retries: Int = 2): String =
        exec(Request.Builder().url(base() + path).post(json.toRequestBody(JSON)), auth, retries)

    private fun exec(rb: Request.Builder, auth: Boolean, retries: Int): String {
        if (auth) {
            val tok = Prefs.token(ctx) ?: throw Unauthorized()
            rb.header("Authorization", "Bearer $tok")
        }
        rb.header("User-Agent", "kidpager-bridge/${BuildConfig.VERSION_NAME}")
        var attempt = 0
        while (true) {
            try {
                http.newCall(rb.build()).execute().use { resp ->
                    val text = resp.body?.string().orEmpty()
                    when {
                        resp.isSuccessful -> return text
                        resp.code == 401 && auth -> {
                            Log.w(TAG, "401 from relay; token cleared, re-pair needed")
                            Prefs.clearToken(ctx)
                            onUnauthorized?.invoke(ctx)
                            throw Unauthorized()
                        }
                        resp.code >= 500 && attempt < retries -> Log.w(TAG, "HTTP ${resp.code}, retry ${attempt + 1}")
                        else -> throw HttpError(resp.code, text)
                    }
                }
            } catch (e: Unauthorized) {
                throw e
            } catch (e: HttpError) {
                throw e
            } catch (e: IOException) {
                if (attempt >= retries) throw e
                Log.w(TAG, "IO ${e.javaClass.simpleName}, retry ${attempt + 1}")
            }
            attempt++
            Thread.sleep(1000L shl (attempt - 1))
        }
    }
}

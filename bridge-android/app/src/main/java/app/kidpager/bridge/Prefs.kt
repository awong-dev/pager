package app.kidpager.bridge

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * Decision 13 ("token in EncryptedSharedPreferences"): relay URL, bridge token and the
 * hand-entered fields the setup screen collects when the phone cannot read them itself.
 */
object Prefs {
    private const val FILE = "bridge_secure"
    @Volatile private var prefs: SharedPreferences? = null

    private fun get(ctx: Context): SharedPreferences {
        prefs?.let { return it }
        synchronized(this) {
            prefs?.let { return it }
            val key = MasterKey.Builder(ctx.applicationContext)
                .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
                .build()
            val p = EncryptedSharedPreferences.create(
                ctx.applicationContext, FILE, key,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
                EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
            )
            prefs = p
            return p
        }
    }

    /** Defaults to BuildConfig.DEFAULT_RELAY_URL (gradle property RELAY_URL, else https://kid-pager.web.app). */
    fun relayUrl(ctx: Context): String =
        get(ctx).getString("relayUrl", null)?.ifBlank { null } ?: BuildConfig.DEFAULT_RELAY_URL.trimEnd('/')
    fun setRelayUrl(ctx: Context, url: String) = get(ctx).edit().putString("relayUrl", url.trimEnd('/')).apply()

    fun token(ctx: Context): String? = get(ctx).getString("token", null)
    fun bridgeId(ctx: Context): String? = get(ctx).getString("bridgeId", null)
    fun setPaired(ctx: Context, bridgeId: String, token: String) =
        get(ctx).edit().putString("bridgeId", bridgeId).putString("token", token).apply()
    /** Decision 2: a 401 means the token is dead; the owner re-pairs. */
    fun clearToken(ctx: Context) = get(ctx).edit().remove("token").apply()
    fun isPaired(ctx: Context) = token(ctx) != null

    fun simNumberOverride(ctx: Context): String? = get(ctx).getString("simNumber", null)?.ifBlank { null }
    fun setSimNumberOverride(ctx: Context, v: String?) = get(ctx).edit().putString("simNumber", v ?: "").apply()
    fun voiceNumber(ctx: Context): String? = get(ctx).getString("voiceNumber", null)?.ifBlank { null }
    fun setVoiceNumber(ctx: Context, v: String?) = get(ctx).edit().putString("voiceNumber", v ?: "").apply()
    fun accountsOverride(ctx: Context): List<String> =
        (get(ctx).getString("accounts", "") ?: "").split(',').map { it.trim() }.filter { it.isNotEmpty() }
    fun setAccountsOverride(ctx: Context, v: String) = get(ctx).edit().putString("accounts", v).apply()

    /** O2: outbox poll period, 30-300 s, default 60. */
    fun pollIntervalSec(ctx: Context): Int = get(ctx).getInt("pollSec", 60).coerceIn(30, 300)
    fun setPollIntervalSec(ctx: Context, v: Int) = get(ctx).edit().putInt("pollSec", v.coerceIn(30, 300)).apply()

    fun fcmToken(ctx: Context): String? = get(ctx).getString("fcmToken", null)
    fun setFcmToken(ctx: Context, v: String?) = get(ctx).edit().putString("fcmToken", v).apply()
}

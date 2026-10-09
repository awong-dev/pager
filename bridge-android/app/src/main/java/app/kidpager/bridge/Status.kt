package app.kidpager.bridge

import android.accounts.AccountManager
import android.app.role.RoleManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.BatteryManager
import android.os.Build
import android.provider.Settings
import android.provider.Telephony
import android.telephony.SubscriptionManager
import android.telephony.TelephonyManager
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Decision 1's `status` block and A2: battery, listener bound, default SMS role, accessibility
 * enabled, Google accounts, SIM number, Voice number (O1: hand-entered), app version; and the
 * `caps` the pair request carries (O1: `sms` = SIM present and role held, `gvoice` = Voice
 * number present, `gchat` = listener bound, `whatsapp` = WhatsApp installed and listener bound (WA1/WA7)).
 */
object Status {
    data class Snapshot(
        val battery: Int,
        val listenerBound: Boolean,
        val smsDefault: Boolean,
        val accessibility: Boolean,
        val accounts: List<String>,
        val simNumber: String?,
        val voiceNumber: String?,
        /** WA7: `com.whatsapp` (or WhatsApp Business) is installed. */
        val whatsappInstalled: Boolean,
        val version: String,
        val fcm: Boolean,
        val pollSec: Int,
    ) {
        val caps get() = RelayClient.Caps(
            sms = simNumber != null && smsDefault,
            gchat = listenerBound,
            gvoice = voiceNumber != null,
            whatsapp = whatsapp,
        )

        /** WA1: the phone can bridge WhatsApp (installed and the listener reads its notifications). */
        val whatsapp get() = whatsappInstalled && listenerBound

        /** Decision 11 heartbeat `status`. */
        fun toJson(): JsonObject = buildJsonObject {
            put("battery", battery)
            put("listenerBound", listenerBound)
            put("smsDefault", smsDefault)
            put("accessibility", accessibility)
            put("accounts", buildJsonArray { accounts.forEach { add(kotlinx.serialization.json.JsonPrimitive(it)) } })
            if (simNumber != null) put("simNumber", simNumber)
            if (voiceNumber != null) put("voiceNumber", voiceNumber)
            put("whatsapp", whatsapp)
            put("version", version)
            put("fcm", fcm)
            put("pollSec", pollSec)
            put("replyActions", ReplyCache.size())
        }
    }

    fun gather(ctx: Context): Snapshot = Snapshot(
        battery = battery(ctx),
        listenerBound = listenerBound(ctx),
        smsDefault = smsDefault(ctx),
        accessibility = accessibilityEnabled(ctx),
        accounts = accounts(ctx),
        simNumber = simNumber(ctx),
        voiceNumber = Prefs.voiceNumber(ctx)?.let { PhoneNumbers.normalize(it) },
        whatsappInstalled = whatsappPackage(ctx) != null,
        version = BuildConfig.VERSION_NAME,
        fcm = BuildConfig.FCM,
        pollSec = Prefs.pollIntervalSec(ctx),
    )

    fun battery(ctx: Context): Int =
        (ctx.getSystemService(Context.BATTERY_SERVICE) as BatteryManager).getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)

    fun listenerBound(ctx: Context): Boolean =
        NotificationManagerCompat.getEnabledListenerPackages(ctx).contains(ctx.packageName)

    fun smsDefault(ctx: Context): Boolean = if (Build.VERSION.SDK_INT >= 29) {
        ctx.getSystemService(RoleManager::class.java)?.isRoleHeld(RoleManager.ROLE_SMS) == true
    } else {
        Telephony.Sms.getDefaultSmsPackage(ctx) == ctx.packageName
    }

    fun accessibilityEnabled(ctx: Context): Boolean {
        val enabled = Settings.Secure.getString(ctx.contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) ?: return false
        val me = "${ctx.packageName}/${BridgeAccessibilityService::class.java.name}"
        val short = "${ctx.packageName}/.${BridgeAccessibilityService::class.java.simpleName}"
        return enabled.split(':').any { it.equals(me, true) || it.equals(short, true) }
    }

    /** WA7: the installed WhatsApp package (`com.whatsapp` first, then Business), or null. */
    fun whatsappPackage(ctx: Context): String? =
        listOf(Targets.WHATSAPP_PKG, Targets.WHATSAPP_BUSINESS_PKG).firstOrNull { pkg ->
            try { ctx.packageManager.getPackageInfo(pkg, 0); true } catch (e: PackageManager.NameNotFoundException) { false }
        }

    fun batteryExempt(ctx: Context): Boolean =
        (ctx.getSystemService(Context.POWER_SERVICE) as android.os.PowerManager).isIgnoringBatteryOptimizations(ctx.packageName)

    /** Google accounts when GET_ACCOUNTS is granted and GMS exposes them; else the setup screen's list. */
    fun accounts(ctx: Context): List<String> {
        val typed = Prefs.accountsOverride(ctx)
        if (typed.isNotEmpty()) return typed
        if (ContextCompat.checkSelfPermission(ctx, android.Manifest.permission.GET_ACCOUNTS) != PackageManager.PERMISSION_GRANTED) return emptyList()
        return try {
            AccountManager.get(ctx).getAccountsByType("com.google").map { it.name }
        } catch (e: Exception) { emptyList() }
    }

    /** SIM number: the setup screen's override, else SubscriptionManager/TelephonyManager (may be empty). */
    fun simNumber(ctx: Context): String? {
        Prefs.simNumberOverride(ctx)?.let { return PhoneNumbers.normalize(it) }
        return PhoneNumbers.normalize(readLine1(ctx))
    }

    @Suppress("MissingPermission", "DEPRECATION")
    fun readLine1(ctx: Context): String? {
        val granted = listOf(android.Manifest.permission.READ_PHONE_NUMBERS, android.Manifest.permission.READ_PHONE_STATE)
            .any { ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED }
        if (!granted) return null
        return try {
            val sm = ctx.getSystemService(SubscriptionManager::class.java)
            val subId = SubscriptionManager.getDefaultSmsSubscriptionId()
            val fromSub = if (Build.VERSION.SDK_INT >= 33 && subId >= 0) sm?.getPhoneNumber(subId) else null
            fromSub?.ifBlank { null }
                ?: sm?.activeSubscriptionInfoList?.firstOrNull()?.number?.ifBlank { null }
                ?: ctx.getSystemService(TelephonyManager::class.java)?.line1Number?.ifBlank { null }
        } catch (e: SecurityException) { null }
    }

    fun describe(s: Snapshot): String = buildString {
        append("battery ${s.battery}%  listener ${s.listenerBound}  smsDefault ${s.smsDefault}  a11y ${s.accessibility}  whatsapp ${s.whatsapp}\n")
        append("accounts ${s.accounts}  sim ${Log.redact(s.simNumber)}  voice ${Log.redact(s.voiceNumber)}  fcm ${s.fcm}  poll ${s.pollSec}s")
    }
}

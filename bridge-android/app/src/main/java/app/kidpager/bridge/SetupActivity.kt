package app.kidpager.bridge

import android.Manifest
import android.app.role.RoleManager
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import app.kidpager.bridge.databinding.ActivitySetupBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.lifecycle.lifecycleScope

/**
 * Decision 13 (setup screen) + O1/O2: relay URL, pairing code -> POST /bridge/pair with
 * accounts, simNumber (auto-filled, editable), voiceNumber (hand-entered), caps; status rows with
 * "Open settings" for Notification access, Default SMS app, battery optimisation, Accessibility;
 * a WhatsApp row (WA7: installed / bridged, no number field);
 * the outbox poll period; a log screen.
 */
class SetupActivity : AppCompatActivity() {
    private lateinit var b: ActivitySetupBinding
    private val runtimePerms = buildList {
        add(Manifest.permission.READ_PHONE_STATE)
        add(Manifest.permission.READ_PHONE_NUMBERS)
        add(Manifest.permission.GET_ACCOUNTS)
        add(Manifest.permission.RECEIVE_SMS)
        add(Manifest.permission.SEND_SMS)
        add(Manifest.permission.READ_SMS)
        add(Manifest.permission.RECEIVE_MMS)
        add(Manifest.permission.RECEIVE_WAP_PUSH)
        if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivitySetupBinding.inflate(layoutInflater)
        setContentView(b.root)
        Notifications.ensureChannels(this)

        b.relayUrl.setText(Prefs.relayUrl(this))
        b.voiceNumber.setText(Prefs.voiceNumber(this) ?: "")
        b.accounts.setText(Prefs.accountsOverride(this).joinToString(", "))
        b.pollSec.setText(Prefs.pollIntervalSec(this).toString())

        b.permissions.setOnClickListener { ActivityCompat.requestPermissions(this, runtimePerms.toTypedArray(), 1) }
        b.pair.setOnClickListener { pair() }
        b.unpair.setOnClickListener { Prefs.clearToken(this); Log.w("setup", "token cleared by user"); refresh() }
        b.save.setOnClickListener { saveFields(); Toast.makeText(this, "saved", Toast.LENGTH_SHORT).show(); refresh() }
        b.startService.setOnClickListener { BridgeService.start(this); refresh() }
        b.heartbeatNow.setOnClickListener { lifecycleScope.launch(Dispatchers.IO) { BridgeService.heartbeat(this@SetupActivity); OutboxWorker.kick() } }
        b.openLog.setOnClickListener { startActivity(Intent(this, LogActivity::class.java)) }

        b.openListener.setOnClickListener { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }
        b.openSmsRole.setOnClickListener {
            if (Build.VERSION.SDK_INT >= 29) {
                val rm = getSystemService(RoleManager::class.java)
                if (rm.isRoleAvailable(RoleManager.ROLE_SMS)) startActivityForResult(rm.createRequestRoleIntent(RoleManager.ROLE_SMS), 2)
                else Toast.makeText(this, "SMS role not available on this device", Toast.LENGTH_LONG).show()
            } else {
                startActivity(Intent(android.provider.Telephony.Sms.Intents.ACTION_CHANGE_DEFAULT).putExtra(android.provider.Telephony.Sms.Intents.EXTRA_PACKAGE_NAME, packageName))
            }
        }
        b.openBattery.setOnClickListener {
            @Suppress("BatteryLife")
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        }
        b.openAccessibility.setOnClickListener { startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)) }
    }

    override fun onResume() { super.onResume(); refresh() }

    private fun saveFields() {
        Prefs.setRelayUrl(this, b.relayUrl.text.toString().trim())
        Prefs.setSimNumberOverride(this, b.simNumber.text.toString().trim().ifBlank { null })
        Prefs.setVoiceNumber(this, b.voiceNumber.text.toString().trim().ifBlank { null })
        Prefs.setAccountsOverride(this, b.accounts.text.toString())
        Prefs.setPollIntervalSec(this, b.pollSec.text.toString().toIntOrNull() ?: 60)
    }

    private fun refresh() {
        val s = Status.gather(this)
        if (b.simNumber.text.isNullOrBlank()) b.simNumber.setText(s.simNumber ?: "")
        b.pairState.text = if (Prefs.isPaired(this)) "Paired as ${Prefs.bridgeId(this)}" else "Not paired"
        b.serviceState.text = if (BridgeService.running) "Service running" else "Service stopped"
        b.listenerState.text = "Notification access: ${if (s.listenerBound) "ON" else "OFF"}"
        b.smsState.text = "Default SMS app: ${if (s.smsDefault) "YES" else "NO"}"
        b.batteryState.text = "Battery optimisation exempt: ${if (Status.batteryExempt(this)) "YES" else "NO"} (${s.battery}%)"
        b.accessibilityState.text = "Accessibility service: ${if (s.accessibility) "ON" else "OFF"}"
        b.whatsappState.text = "WhatsApp: installed ${if (s.whatsappInstalled) "YES" else "NO"}, bridged ${if (s.whatsapp) "YES" else "NO (needs notification access)"}"
        b.capsState.text = "caps: sms=${s.caps.sms} gchat=${s.caps.gchat} gvoice=${s.caps.gvoice} whatsapp=${s.caps.whatsapp}  fcm=${s.fcm}  accounts=${s.accounts.size}"
    }

    /** Decision 2: POST /bridge/pair {code, version, accounts, simNumber?, voiceNumber?, caps}; the token is returned once. */
    private fun pair() {
        saveFields()
        val code = b.pairCode.text.toString().trim()
        if (Prefs.relayUrl(this).isBlank() || code.length < 4) { Toast.makeText(this, "relay URL and code needed", Toast.LENGTH_SHORT).show(); return }
        val s = Status.gather(this)
        val req = RelayClient.PairRequest(
            code = code,
            version = BuildConfig.VERSION_NAME,
            accounts = s.accounts.take(10), // relay PairRequest caps accounts at 10
            simNumber = s.simNumber,
            voiceNumber = s.voiceNumber,
            caps = s.caps,
        )
        b.pair.isEnabled = false
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                try {
                    val r = RelayClient(this@SetupActivity).pair(req)
                    Prefs.setPaired(this@SetupActivity, r.bridgeId, r.token)
                    Notifications.clearRepair(this@SetupActivity)
                    Log.i("setup", "paired as ${r.bridgeId} caps=${s.caps}")
                    BridgeService.start(this@SetupActivity)
                    BridgeService.heartbeat(this@SetupActivity)
                    "paired as ${r.bridgeId}"
                } catch (e: RelayClient.HttpError) {
                    Log.w("setup", "pair failed ${e.code}: ${e.body.take(120)}")
                    when (e.code) {
                        404 -> "code expired or already used"
                        429 -> "too many attempts; wait a minute"
                        409 -> e.detail ?: "pair rejected: ${e.body.take(80)}"
                        422 -> "relay rejected a field: ${e.reason().take(80)}"
                        else -> e.detail?.let { "pair failed: $it" } ?: "pair failed: HTTP ${e.code}"
                    }
                } catch (e: Exception) {
                    Log.e("setup", "pair failed", e)
                    "pair failed: ${e.message}"
                }
            }
            Toast.makeText(this@SetupActivity, result, Toast.LENGTH_LONG).show()
            b.pair.isEnabled = true
            b.pairCode.setText("")
            refresh()
        }
    }
}

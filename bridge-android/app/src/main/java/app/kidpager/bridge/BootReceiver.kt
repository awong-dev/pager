package app.kidpager.bridge

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Decision 13: boot receiver (RECEIVE_BOOT_COMPLETED) restarts the sticky service; also after an update. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        Log.i("boot", "${intent.action}; starting service")
        BridgeService.start(context)
    }
}

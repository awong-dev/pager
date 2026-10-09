package app.kidpager.bridge

import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage

/**
 * A5 (push build, BuildConfig.FCM = true): a data push `{kind: "outbox"}` wakes the outbox
 * worker at once (O2); a new token is stored and sent with the next heartbeat (decision 11).
 */
class FcmService : FirebaseMessagingService() {
    override fun onMessageReceived(message: RemoteMessage) {
        if (message.data["kind"] == "outbox") {
            Log.i("fcm", "outbox push")
            if (!BridgeService.running) BridgeService.start(this)
            OutboxWorker.kick()
        }
    }

    override fun onNewToken(token: String) {
        Prefs.setFcmToken(this, token)
        Log.i("fcm", "new token")
        Thread { BridgeService.heartbeat(this) }.start()
    }
}

package app.kidpager.bridge

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity

/**
 * Decision 13 ("a dummy ACTION_SENDTO activity so the role is grantable"): nothing to compose on
 * a headless phone; the activity exists for the SMS role check and finishes at once.
 */
class ComposeActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i("compose", "SENDTO ignored (headless)")
        finish()
    }
}

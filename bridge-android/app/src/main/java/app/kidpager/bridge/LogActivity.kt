package app.kidpager.bridge

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import app.kidpager.bridge.databinding.ActivityLogBinding

/** Decision 13: the log screen over Log's 500-line ring buffer, live-updating. */
class LogActivity : AppCompatActivity() {
    private lateinit var b: ActivityLogBinding
    private val listener: (String) -> Unit = { line -> runOnUiThread { b.text.append("\n$line"); scrollDown() } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityLogBinding.inflate(layoutInflater)
        setContentView(b.root)
        b.text.text = Log.snapshot().joinToString("\n")
        b.replyCache.setOnClickListener { b.text.append("\n-- reply cache --\n" + ReplyCache.describe().ifEmpty { "(empty)" }); scrollDown() }
        b.status.setOnClickListener { b.text.append("\n-- status --\n" + Status.describe(Status.gather(this))); scrollDown() }
        scrollDown()
    }

    private fun scrollDown() = b.scroll.post { b.scroll.fullScroll(android.view.View.FOCUS_DOWN) }

    override fun onStart() { super.onStart(); Log.addListener(listener) }
    override fun onStop() { Log.removeListener(listener); super.onStop() }
}

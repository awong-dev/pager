package app.kidpager.bridge

/**
 * Decision 13 (Voice bullet, "peer's number parsed from the title/sender when present") and
 * decision 5 (`sender.phone`, relay-normalised): a pure, unit-testable normaliser so the
 * listener and the SMS receiver agree. Mirrors PhoneNumberUtils.normalizeNumber plus a
 * North-American E.164 default; the relay re-normalises with libphonenumber, so this only
 * has to be *plausible*, never authoritative.
 */
object PhoneNumbers {
    private val digitsAndPlus = Regex("[^0-9+]")

    /** `+15551234567` for anything that looks like a phone number, else null. */
    fun normalize(raw: String?): String? {
        if (raw.isNullOrBlank()) return null
        var s = raw.trim()
        if (s.startsWith("tel:")) s = s.removePrefix("tel:")
        val cleaned = digitsAndPlus.replace(s, "")
        val plus = cleaned.startsWith("+")
        val digits = cleaned.filter { it.isDigit() }
        if (digits.length < 7 || digits.length > 15) return null
        // Letters in the raw string that are not formatting mean it is a name, not a number.
        if (s.any { it.isLetter() }) return null
        return when {
            plus -> "+$digits"
            digits.length == 11 && digits.startsWith("1") -> "+$digits"
            digits.length == 10 -> "+1$digits"
            else -> "+$digits"
        }
    }

    /** O4: the first E.164-looking run inside an opaque id (`t.+15551234567`, `tel:+1555...`). */
    fun extractE164(s: String?): String? {
        if (s.isNullOrBlank()) return null
        val m = Regex("\\+?[0-9][0-9 ()-]{8,20}").find(s) ?: return null
        return normalize(m.value)
    }

    /** True when `s` is a number rather than a contact name (Voice titles are one or the other). */
    fun looksLikeNumber(s: String?): Boolean = normalize(s) != null
}

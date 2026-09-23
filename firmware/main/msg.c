// msg.c — message storage, dedup, ack state machine, retry pump.
//
// Authority: docs/PROTOCOL.md §9 (storage contract), §4 (ack state machine),
// §3.1 (body limits). See msg.h for the ownership/locking discipline, the
// deliberate created_us (monotonic) deviation from the doc's literal
// "created_epoch" field name, and the F6.4 NVS-vs-RTC split.
//
// All power-effect comments are PENDING_HW.

#include "msg.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Composer buffer (docs/DEVICE_PLAN.md §5.2/§9.4). No ESP-IDF dependency —
// compiled on the host by firmware/host/test_msg.c (same `#ifdef ESP_PLATFORM`
// split as input.c/auth.c). See msg.h's doc comment on why
// msg_composer_push_char() keeps its one-byte-at-a-time signature and
// instead buffers an in-progress UTF-8 sequence internally, committing (or
// refusing) a whole code point atomically once it is complete.
// ---------------------------------------------------------------------------

static char s_composer[MSG_COMPOSER_MAX];
static uint16_t s_composer_len = 0;        // bytes committed
static uint16_t s_composer_codepoints = 0; // code points committed

// An in-progress, not-yet-committed multi-byte UTF-8 sequence.
static uint8_t s_pending[4];
static uint8_t s_pending_len = 0;  // bytes buffered so far
static uint8_t s_pending_want = 0; // bytes this sequence needs in total, 0 = none in progress

static bool is_utf8_cont(uint8_t b) { return (b & 0xC0) == 0x80; }

static uint8_t utf8_seq_len(uint8_t lead)
{
    if ((lead & 0x80) == 0x00) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1; // invalid lead byte: never wedge the composer, treat as its own code point
}

void msg_composer_reset(void)
{
    s_composer[0] = '\0';
    s_composer_len = 0;
    s_composer_codepoints = 0;
    s_pending_len = 0;
    s_pending_want = 0;
}

bool msg_composer_push_char(char c)
{
    uint8_t b = (uint8_t) c;

    if (s_pending_want > 0 && !is_utf8_cont(b)) {
        // The previous partial sequence was malformed (should not happen
        // from a well-formed IME) — discard it rather than wedge the
        // composer, and reinterpret `b` as the start of a fresh code point.
        s_pending_want = 0;
        s_pending_len = 0;
    }

    if (s_pending_want == 0) {
        s_pending_want = utf8_seq_len(b);
        s_pending_len = 0;
    }
    s_pending[s_pending_len++] = b;

    if (s_pending_len < s_pending_want) {
        return true; // sequence still incomplete; nothing to check yet
    }

    uint8_t want = s_pending_want;
    s_pending_want = 0;

    if (s_composer_codepoints >= MSG_COMPOSER_MAX_CODEPOINTS ||
        (size_t) s_composer_len + want > MSG_COMPOSER_MAX - 1) {
        return false; // refuse the whole code point atomically, never a partial write, §9.4
    }

    memcpy(&s_composer[s_composer_len], s_pending, want);
    s_composer_len = (uint16_t) (s_composer_len + want);
    s_composer[s_composer_len] = '\0';
    s_composer_codepoints++;
    return true;
}

bool msg_composer_backspace(void)
{
    // Cancel any in-progress incomplete sequence first (defensive: in
    // practice scr_chat.c's byte loop always resolves a sequence fully
    // before another key can be dispatched).
    if (s_pending_want > 0) {
        s_pending_want = 0;
        s_pending_len = 0;
        return true;
    }
    if (s_composer_len == 0) {
        return false;
    }
    size_t cut = s_composer_len;
    do {
        cut--;
    } while (cut > 0 && is_utf8_cont((uint8_t) s_composer[cut]));
    s_composer_len = (uint16_t) cut;
    s_composer[s_composer_len] = '\0';
    if (s_composer_codepoints > 0) {
        s_composer_codepoints--;
    }
    return true;
}

const char *msg_composer_text(void) { return s_composer; }
uint16_t msg_composer_len(void) { return s_composer_len; }
uint16_t msg_composer_codepoint_count(void) { return s_composer_codepoints; }

// ---------------------------------------------------------------------------
// msghist record codec (owner task 2026-09-20, "keep recent messages across
// reboots") — pure logic, no ESP-IDF/NVS dependency, host-tested by
// firmware/host/test_msg.c the same way the composer above is (this whole
// section sits above msg.c's own `#ifdef ESP_PLATFORM` split). See msg.h's
// long comment above MSGHIST_REC_VERSION for the on-flash design rationale;
// this section is only the byte-level codec plus the two other bits of pure
// arithmetic (which ack state is worth a second write, which decoded record
// is newest) that the ESP-only history_restore()/mark_common() etc. below
// build on.
// ---------------------------------------------------------------------------

// Bitwise CRC32 (polynomial 0xEDB88320 — the same one zlib/PNG/
// esp_rom_crc32_le() use, but computed independently here without a
// lookup table): this guards a few-hundred-byte record written at most a
// couple of times per message, not a hot path, so trading a 256-entry
// table for a few dozen extra cycles is the right call on a build that
// already avoids all dynamic allocation. Not the same digest as
// id_digest()'s esp_rom_crc32_le() call below, or any wire value — purely
// private to this file's own corruption check.
static uint32_t msghist_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t) (-(int32_t) (crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// --- little-endian bounded-write/-read primitives used by the codec below.
// Each returns false (writing/reading nothing) the instant it would run
// past `cap`/`len` — msghist_record_encode()/_decode() lean on this so
// they never need their own separate bounds check per field. ---

static bool w_u8(uint8_t *out, size_t cap, size_t *off, uint8_t v)
{
    if (*off + 1 > cap) {
        return false;
    }
    out[(*off)++] = v;
    return true;
}

static bool w_u16(uint8_t *out, size_t cap, size_t *off, uint16_t v)
{
    if (*off + 2 > cap) {
        return false;
    }
    out[(*off)++] = (uint8_t) (v & 0xFF);
    out[(*off)++] = (uint8_t) (v >> 8);
    return true;
}

static bool w_u32(uint8_t *out, size_t cap, size_t *off, uint32_t v)
{
    if (*off + 4 > cap) {
        return false;
    }
    out[(*off)++] = (uint8_t) (v & 0xFF);
    out[(*off)++] = (uint8_t) ((v >> 8) & 0xFF);
    out[(*off)++] = (uint8_t) ((v >> 16) & 0xFF);
    out[(*off)++] = (uint8_t) ((v >> 24) & 0xFF);
    return true;
}

static bool w_i64(uint8_t *out, size_t cap, size_t *off, int64_t v)
{
    uint64_t u = (uint64_t) v; // two's-complement reinterpret; msg.c-private wire, no cross-platform contract
    if (!w_u32(out, cap, off, (uint32_t) (u & 0xFFFFFFFFu))) {
        return false;
    }
    return w_u32(out, cap, off, (uint32_t) (u >> 32));
}

static bool w_bytes(uint8_t *out, size_t cap, size_t *off, const void *src, size_t n)
{
    if (*off + n > cap) {
        return false;
    }
    memcpy(out + *off, src, n);
    *off += n;
    return true;
}

// One-byte length prefix + bytes — every id/from/to field here is at most
// 16 bytes (MSG_ID_MAX/MSG_FROM_MAX/MSG_TO_MAX), well under 255.
static bool w_str_field(uint8_t *out, size_t cap, size_t *off, const char *s)
{
    size_t len = strlen(s);
    if (len > 255) {
        len = 255; // defensive; never actually reached by this file's own callers
    }
    if (!w_u8(out, cap, off, (uint8_t) len)) {
        return false;
    }
    return w_bytes(out, cap, off, s, len);
}

static bool r_u8(const uint8_t *buf, size_t len, size_t *off, uint8_t *v)
{
    if (*off + 1 > len) {
        return false;
    }
    *v = buf[(*off)++];
    return true;
}

static bool r_u16(const uint8_t *buf, size_t len, size_t *off, uint16_t *v)
{
    if (*off + 2 > len) {
        return false;
    }
    *v = (uint16_t) (buf[*off] | (buf[*off + 1] << 8));
    *off += 2;
    return true;
}

static bool r_u32(const uint8_t *buf, size_t len, size_t *off, uint32_t *v)
{
    if (*off + 4 > len) {
        return false;
    }
    *v = (uint32_t) buf[*off] | ((uint32_t) buf[*off + 1] << 8) | ((uint32_t) buf[*off + 2] << 16) |
         ((uint32_t) buf[*off + 3] << 24);
    *off += 4;
    return true;
}

static bool r_i64(const uint8_t *buf, size_t len, size_t *off, int64_t *v)
{
    uint32_t lo, hi;
    if (!r_u32(buf, len, off, &lo) || !r_u32(buf, len, off, &hi)) {
        return false;
    }
    uint64_t u = ((uint64_t) hi << 32) | lo;
    *v = (int64_t) u;
    return true;
}

// Bounds `dst_cap` (a MSG_*_MAX-sized buffer, cap includes the NUL) against
// the encoded length prefix and rejects (does not truncate) anything that
// would not fit — an oversized length prefix here can only mean a corrupt
// or foreign record, and this file's own rule throughout is "skip the whole
// record", never "truncate and keep going" (msg.h's decode contract).
static bool r_str_field(const uint8_t *buf, size_t len, size_t *off, char *dst, size_t dst_cap)
{
    uint8_t slen;
    if (!r_u8(buf, len, off, &slen)) {
        return false;
    }
    if (*off + slen > len || (size_t) slen > dst_cap - 1) {
        return false;
    }
    memcpy(dst, buf + *off, slen);
    dst[slen] = '\0';
    *off += slen;
    return true;
}

size_t msghist_record_encode(const msg_t *m, uint32_t seq, uint8_t *out, size_t out_cap)
{
    if (!m || !out) {
        return 0;
    }
    size_t off = 0;
    if (!w_u8(out, out_cap, &off, MSGHIST_REC_VERSION) || !w_u32(out, out_cap, &off, seq) ||
        !w_i64(out, out_cap, &off, m->ts) || !w_u8(out, out_cap, &off, m->dir) ||
        !w_u8(out, out_cap, &off, m->ack_state) || !w_u8(out, out_cap, &off, m->flags) ||
        !w_str_field(out, out_cap, &off, m->id) || !w_str_field(out, out_cap, &off, m->from) ||
        !w_str_field(out, out_cap, &off, m->to) || !w_str_field(out, out_cap, &off, m->sndr) ||
        !w_u16(out, out_cap, &off, m->body_len) ||
        !w_bytes(out, out_cap, &off, m->body, m->body_len)) {
        return 0;
    }
    uint32_t crc = msghist_crc32(out, off);
    if (!w_u32(out, out_cap, &off, crc)) {
        return 0;
    }
    return off;
}

bool msghist_record_decode(const uint8_t *buf, size_t len, msg_t *out, uint32_t *out_seq)
{
    if (!buf || !out) {
        return false;
    }
    size_t off = 0;
    uint8_t version;
    uint32_t seq;
    int64_t ts;
    uint8_t dir, ack_state, flags;
    char id[MSG_ID_MAX], from[MSG_FROM_MAX], to[MSG_TO_MAX], sndr[MSG_FROM_MAX];
    uint16_t body_len;

    // G7: accept version 1 (pre-`sndr`) and version 2 (adds `sndr`) alike —
    // rejecting version 1 here would drop every message a pre-G7 firmware
    // ever persisted on the first boot after the upgrade (msg.h's own
    // comment on MSGHIST_REC_VERSION).
    if (!r_u8(buf, len, &off, &version) || version < 1 || version > MSGHIST_REC_VERSION) {
        return false;
    }
    if (!r_u32(buf, len, &off, &seq) || !r_i64(buf, len, &off, &ts) ||
        !r_u8(buf, len, &off, &dir) || dir > 1 || !r_u8(buf, len, &off, &ack_state) ||
        !r_u8(buf, len, &off, &flags) || !r_str_field(buf, len, &off, id, sizeof(id)) ||
        !r_str_field(buf, len, &off, from, sizeof(from)) ||
        !r_str_field(buf, len, &off, to, sizeof(to))) {
        return false;
    }
    sndr[0] = '\0';
    if (version >= 2 && !r_str_field(buf, len, &off, sndr, sizeof(sndr))) {
        return false;
    }
    if (!r_u16(buf, len, &off, &body_len) || (size_t) body_len > MSG_RAM_BODY_MAX - 1 ||
        off + body_len > len) {
        return false;
    }
    const uint8_t *body_ptr = buf + off;
    off += body_len;

    uint32_t stored_crc;
    if (!r_u32(buf, len, &off, &stored_crc)) {
        return false;
    }
    if (msghist_crc32(buf, off - 4) != stored_crc) {
        return false; // torn write or foreign data — skip this slot, never fatal
    }

    memset(out, 0, sizeof(*out));
    out->ts = ts;
    memcpy(out->id, id, sizeof(id));
    memcpy(out->from, from, sizeof(from));
    memcpy(out->to, to, sizeof(to));
    memcpy(out->sndr, sndr, sizeof(sndr));
    memcpy(out->body, body_ptr, body_len);
    out->body[body_len] = '\0';
    out->body_len = body_len;
    out->dir = dir;
    out->ack_state = ack_state;
    out->flags = flags;
    out->in_use = true;
    out->hist_seq = seq;
    if (out_seq) {
        *out_seq = seq;
    }
    return true;
}

bool msghist_is_terminal_ack(uint8_t dir, uint8_t ack_state)
{
    if (dir == (uint8_t) MSG_DIR_DOWN) {
        return ack_state == MSG_ACK_READ;
    }
    return ack_state == MSG_ACK_UP_SENT || ack_state == MSG_ACK_UP_FAILED;
}

int msghist_restore_order(const uint32_t *seqs, int n, int *order_out, int max_out)
{
    if (n < 0) {
        n = 0;
    }
    if (n > max_out) {
        n = max_out;
    }
    if (n > MSG_THREAD_DEPTH) {
        n = MSG_THREAD_DEPTH; // this file's own only caller never exceeds this; defensive floor for the `used[]` array below
    }
    bool used[MSG_THREAD_DEPTH] = { 0 };
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used[i]) {
                continue;
            }
            if (best < 0 || seqs[i] > seqs[best]) {
                best = i;
            }
        }
        used[best] = true;
        order_out[k] = best;
    }
    return n;
}

#ifdef ESP_PLATFORM

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h" // nvs_flash_init_partition()/nvs_flash_erase_partition(), msghist (§ below)

// F3.6 (docs/PROTOCOL.md §14, §10 keymap): signing/verification and the CBOR
// codec every publish now goes through. ident.h is needed only for the
// read-only IDENT_FLAG_REQ_SIG check and ident_get_n_epoch() — this file
// still does no ident NVS I/O of its own (see msg_bind_auth()'s header
// comment for where n_epoch persistence on wrap actually happens). F6.4:
// net.h/cbor.h/auth.h are now the ONLY wire codec — every device decodes
// CBOR, docs/DEVICE_PLAN.md H14 — and nvs.h is new, for namespace `msgq`
// (docs/PROTOCOL.md §9.2/§9.4).
#include "auth.h"
#include "cbor.h"
#include "ident.h"
#include "net.h"
// v0.2 §6 (device-direct SMS): msg_pump()'s own tail call into
// sms_try_publish_one() (see its call site's own comment) is the ONLY
// reason this file knows sms.c exists at all — every other SMS concern
// (allow-list, encoding, the audit ring itself) is entirely sms.c's own.
#include "sms.h"

static const char *TAG = "msg";

// PROTOCOL.md §10 envelope keymap — the subset this file writes/reads.
// Key 13 (sig) is never written/read via cbor_w_*/cbor_r_* here: auth_sign()
// appends it as ten raw bytes and auth_verify() strips those same ten bytes
// before this file ever sees the buffer (docs/DEVICE_PLAN.md §2.4's
// "no re-serialisation" rule).
#define MK_V 0
#define MK_ID 1
#define MK_TS 2
#define MK_FROM 3
#define MK_BODY 4
#define MK_ACK 5
#define MK_KIND 6
#define MK_TO 7
#define MK_N 12
#define MK_SNDR 51 // G7 (docs/GROUP_CHAT_DESIGN.md §4): group-message author alias, down-only

// ---------------------------------------------------------------------------
// RTC wiring (msg.h: msg_bind_rtc()).
// ---------------------------------------------------------------------------

// Real hardware finding: setup_run() (F3.5) calls ui_init() standalone,
// before modes_boot() (and therefore msg_bind_rtc()) ever runs -- by
// design, setup.c predates and is meant to run independently of the rest
// of the app. F6.3's later scr_home.c calls msg_thread_count() while
// building the Home screen's menu regardless, which used to jump through
// a NULL s_lock()/s_unlock() (Guru Meditation InstrFetchProhibited,
// confirmed live). Defaulting to a no-op keeps every accessor safe to call
// pre-bind and correctly reports "nothing yet", which is the truthful
// answer at that point anyway.
static void msg_rtc_lock_noop(void) {}

static msg_rtc_t *s_rtc = NULL;
static msg_rtc_lock_fn s_lock = msg_rtc_lock_noop;
static msg_rtc_unlock_fn s_unlock = msg_rtc_lock_noop;
static msg_rtc_save_fn s_save = msg_rtc_lock_noop;

void msg_bind_rtc(msg_rtc_t *rtc, msg_rtc_lock_fn lock, msg_rtc_unlock_fn unlock,
                   msg_rtc_save_fn save)
{
    s_rtc = rtc;
    s_lock = lock;
    s_unlock = unlock;
    s_save = save;
}

// F3.6: auth_rtc_t binding — see msg.h's msg_bind_auth() doc comment.
static auth_rtc_t *s_auth_rtc = NULL;
static msg_epoch_wrap_fn s_on_epoch_wrap = NULL;

void msg_bind_auth(auth_rtc_t *rtc, msg_epoch_wrap_fn on_wrap)
{
    s_auth_rtc = rtc;
    s_on_epoch_wrap = on_wrap;
}

void msg_count_malformed(void)
{
    s_lock();
    s_rtc->malformed_drops++;
    s_save();
    s_unlock();
}

// ---------------------------------------------------------------------------
// RAM-resident state (msg.c's own .bss, PROTOCOL.md §9.5). Guarded by the
// same lock as the RTC struct — see msg.h's header comment for why.
// ---------------------------------------------------------------------------

static msg_t s_thread[MSG_THREAD_DEPTH];

static bool s_history_lost_pending = false;
static char s_last_ingest_id[MSG_ID_MAX] = "";

const char *msg_last_ingest_id(void) { return s_last_ingest_id; }

// README R6 fix: dedicated snapshot slots msg_thread_at()/msg_newest_unread()
// copy into under the lock before returning — see msg.h's doc comment.
static msg_t s_thread_at_snapshot;
static msg_t s_unread_snapshot;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// PROTOCOL.md §9.3: parse the id's own 32 bits of entropy if it matches the
// m_/u_ + 8 hex shape, else crc32 the string. A real digest of 0 is stored
// as 1 (0 means empty slot).
static uint32_t id_digest(const char *id)
{
    size_t len = strlen(id);
    if (len == 10 && (id[0] == 'm' || id[0] == 'u') && id[1] == '_') {
        bool all_hex = true;
        for (size_t i = 2; i < 10; i++) {
            if (!isxdigit((unsigned char) id[i])) {
                all_hex = false;
                break;
            }
        }
        if (all_hex) {
            uint32_t v = (uint32_t) strtoul(id + 2, NULL, 16);
            return v == 0 ? 1u : v;
        }
    }
    uint32_t v = esp_rom_crc32_le(0, (const uint8_t *) id, len);
    return v == 0 ? 1u : v;
}

static bool seen_contains_locked(uint32_t digest)
{
    for (int i = 0; i < MSG_SEEN_IDS_MAX; i++) {
        if (s_rtc->seen_ids[i] == digest) {
            return true;
        }
    }
    return false;
}

static void seen_add_locked(uint32_t digest)
{
    s_rtc->seen_ids[s_rtc->seen_head] = digest;
    s_rtc->seen_head = (s_rtc->seen_head + 1) % MSG_SEEN_IDS_MAX;
}

// Shift s_thread down and insert at index 0 (newest-first ring, evicts the
// oldest entry once full). Caller holds the lock.
static void thread_insert_locked(const msg_t *entry)
{
    memmove(&s_thread[1], &s_thread[0], sizeof(msg_t) * (MSG_THREAD_DEPTH - 1));
    s_thread[0] = *entry;
    s_thread[0].in_use = true;
}

static msg_t *thread_find_locked(const char *id, msg_dir_t dir)
{
    for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
        if (s_thread[i].in_use && s_thread[i].dir == (uint8_t) dir &&
            strncmp(s_thread[i].id, id, MSG_ID_MAX) == 0) {
            return &s_thread[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// NVS partition `msghist` (owner task 2026-09-20; msg.h's long comment above
// MSGHIST_REC_VERSION has the full design rationale). Separate from the
// main `nvs` partition/namespace `msgq` below on purpose: a 128 KB
// dedicated partition (firmware/partitions.csv) so this feature can never
// starve identity/msgq/sms/book's own 24 kB `nvs` partition, and a
// dedicated nvs_flash_init_partition() so a missing/corrupt partition on a
// pager whose table hasn't been re-flashed yet degrades to "log once, run
// RAM-only" rather than touching (or being touched by) anything else NVS
// does. Every function here does its own nvs_open_from_partition()/
// nvs_close() and is always called with the RTC/RAM lock RELEASED (flash
// I/O), same discipline as the `msgq` helpers just below.
// ---------------------------------------------------------------------------

#define MSGHIST_PART "msghist"
#define MSGHIST_NS "msghist"

static bool s_hist_available = false;
// Real seq numbers start at 1 (0 = "never persisted", msg.h's msg_t.hist_seq
// comment); history_restore() below overwrites this with (max seq found)+1
// when there is anything to restore, so numbering survives a cold boot too.
static uint32_t s_hist_next_seq = 1;

// Power/flash effect: none by itself — nvs_flash_init_partition() only
// mounts/validates the NVS page structure already on flash (or, on a
// factory-blank 128 KB region, recognises it as an empty valid page set;
// no erase needed in that common case). The erase-and-retry fallback below
// (mirrors main.c's own boilerplate for the main `nvs` partition) only
// costs a real erase cycle on the rare "wrong NVS version" or genuinely
// torn page case. Called once, from msg_init(), before anything reads the
// thread.
static void history_mount(void)
{
    esp_err_t err = nvs_flash_init_partition(MSGHIST_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase_partition(MSGHIST_PART) == ESP_OK) {
            err = nvs_flash_init_partition(MSGHIST_PART);
        }
    }
    if (err != ESP_OK) {
        // Fails open (msg.h/module comment, owner instruction: this feature
        // must never be able to stop a page from being received, rendered
        // or acked): the overwhelmingly common cause is a pager whose
        // partition table predates this feature (esp_partition_find() in
        // nvs_flash_init_partition() returns ESP_ERR_NOT_FOUND for a
        // partition label the table doesn't have) — logged once at INFO,
        // not WARN/ERROR, since that is an expected state for an
        // already-deployed pager, not a fault.
        ESP_LOGI(TAG, "msghist unavailable (0x%x) - message history is RAM-only this boot", err);
        s_hist_available = false;
        return;
    }
    s_hist_available = true;
}

// Allocates the next persisted-history sequence number, or 0 ("do not
// persist this entry") if msghist isn't available. Shares msg.c's existing
// cross-task RAM lock (msg.h's header comment: "the only cross-task
// primitive in this design") to serialise s_hist_next_seq++ the same way
// every other msg.c static already is, even though s_hist_next_seq is not
// itself RTC-resident.
static uint32_t history_next_seq(void)
{
    if (!s_hist_available) {
        return 0;
    }
    s_lock();
    uint32_t seq = s_hist_next_seq++;
    s_unlock();
    return seq;
}

// Writes (or overwrites) `m`'s slot, keyed by its own hist_seq — see msg.h's
// module comment for why this is at most 2 calls per message's whole life
// (once at insert, once more at msghist_is_terminal_ack()) rather than one
// call per ring-aging step. No-op if `m` was never assigned a hist_seq
// (persistence unavailable, or a RECOVERED entry re-inserted by the msgq
// fallback paths below — those must never re-persist, see their own
// comments). Always called with the lock released (flash I/O).
static void history_write_entry(const msg_t *m)
{
    if (!s_hist_available || m->hist_seq == 0) {
        return;
    }
    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = msghist_record_encode(m, m->hist_seq, buf, sizeof(buf));
    if (len == 0) {
        ESP_LOGD(TAG, "msghist: encode failed for id=%s (should not happen)", m->id);
        return; // fail open: this message just isn't persisted this time
    }
    char key[8];
    snprintf(key, sizeof(key), "m%u", (unsigned) (m->hist_seq % MSG_THREAD_DEPTH));

    nvs_handle_t h;
    if (nvs_open_from_partition(MSGHIST_PART, MSGHIST_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, key, buf, len) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// Called once from msg_init(), before the warm/cold msgq recovery paths
// (which only reconstruct in-flight pending-reply/unread rows) and before
// anything else reads the thread. Reads all MSG_THREAD_DEPTH possible
// slots, decodes and validates each independently (a corrupt or
// old-version slot is skipped, never fatal — msg.h's decode contract),
// and restores newest-first by `seq` via msghist_restore_order() (pure,
// host-tested). Leaves s_thread untouched (all-zero, from msg_init()'s own
// memset) if msghist is unavailable or nothing valid is found — that is
// exactly the "first boot / erased / corrupt" case that sets
// s_history_lost_pending below.
static void history_restore(void)
{
    if (!s_hist_available) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(MSGHIST_PART, MSGHIST_NS, NVS_READONLY, &h) != ESP_OK) {
        return; // namespace not created yet: msghist mounted fine but nothing was ever written
    }

    static msg_t decoded[MSG_THREAD_DEPTH]; // static: this runs once at boot, keep it off the stack
    uint32_t seqs[MSG_THREAD_DEPTH];
    int n = 0;
    uint32_t max_seq = 0;

    for (int slot = 0; slot < MSG_THREAD_DEPTH; slot++) {
        char key[8];
        snprintf(key, sizeof(key), "m%u", (unsigned) slot);
        uint8_t buf[MSGHIST_REC_MAX];
        size_t len = sizeof(buf);
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK) {
            continue; // fewer than 32 messages ever persisted, or a gap: not an error
        }
        uint32_t seq;
        if (!msghist_record_decode(buf, len, &decoded[n], &seq)) {
            ESP_LOGI(TAG, "msghist: slot %d failed to decode (corrupt or old version), skipped",
                     slot);
            continue;
        }
        seqs[n] = seq;
        if (seq > max_seq) {
            max_seq = seq;
        }
        n++;
    }
    nvs_close(h);

    if (n == 0) {
        return;
    }

    int order[MSG_THREAD_DEPTH];
    int fill = msghist_restore_order(seqs, n, order, MSG_THREAD_DEPTH);

    s_lock();
    for (int i = 0; i < fill; i++) {
        s_thread[i] = decoded[order[i]];
        // Restored across a reset, same meaning §9.6 already gives this
        // flag for the msgq-only recovery paths below.
        s_thread[i].flags |= MSG_F_RECOVERED;
    }
    s_hist_next_seq = max_seq + 1;
    s_unlock();

    ESP_LOGI(TAG, "msghist: restored %d of %d decoded record(s), newest seq=%u", fill, n,
             (unsigned) max_seq);
}

// Owner task: factory reset must erase the persisted history too. Erases
// every "m0".."m31" key it can find (nvs_erase_all() is simplest and
// correct here: this whole namespace holds nothing but history rows) plus,
// for the same "child's private messages" reason, the msgq pending-reply/
// unread bodies (msgq_erase_reply()/msgq_erase_unread(), defined below —
// forward-declared here since C requires it). No-op if msghist was never
// available. scr_device.c calls this immediately before ident_erase() +
// esp_restart(); it does not itself touch s_thread/RTC (the restart does).
static void msgq_erase_unread(void); // fwd decl: defined further down this file
static void msgq_erase_reply(int slot); // fwd decl: defined further down this file

void msg_history_erase(void)
{
    if (s_hist_available) {
        nvs_handle_t h;
        if (nvs_open_from_partition(MSGHIST_PART, MSGHIST_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    msgq_erase_unread();
    for (int i = 0; i < MSG_PENDING_UP_MAX; i++) {
        msgq_erase_reply(i);
    }
}

// ---------------------------------------------------------------------------
// NVS namespace `msgq` (docs/PROTOCOL.md §9.2/§9.4): full-fidelity bodies
// for a pending reply (per pending_up slot) and the newest unread down
// message. Every function here does its own nvs_open()/nvs_close() and is
// always called with the RTC/RAM lock RELEASED (flash I/O; matches
// msg_pump()'s existing no-I/O-under-lock discipline for net_publish_raw()).
// ---------------------------------------------------------------------------

#define MSGQ_NS "msgq"

static bool msgq_open(nvs_open_mode_t mode, nvs_handle_t *h)
{
    esp_err_t err = nvs_open(MSGQ_NS, mode, h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\") failed: 0x%x", MSGQ_NS, err);
        return false;
    }
    return true;
}

static void reply_keys(int slot, char *kid, size_t kid_cap, char *kto, size_t kto_cap,
                        char *kbody, size_t kbody_cap)
{
    snprintf(kid, kid_cap, "r%did", slot);
    snprintf(kto, kto_cap, "r%dto", slot);
    snprintf(kbody, kbody_cap, "r%dbody", slot);
}

static bool msgq_write_reply(int slot, const char *id, const char *to, const char *body)
{
    char kid[8], kto[8], kbody[8];
    reply_keys(slot, kid, sizeof(kid), kto, sizeof(kto), kbody, sizeof(kbody));

    nvs_handle_t h;
    if (!msgq_open(NVS_READWRITE, &h)) {
        return false;
    }
    bool ok = nvs_set_str(h, kid, id) == ESP_OK && nvs_set_str(h, kto, to) == ESP_OK &&
              nvs_set_str(h, kbody, body) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static void msgq_erase_reply(int slot)
{
    char kid[8], kto[8], kbody[8];
    reply_keys(slot, kid, sizeof(kid), kto, sizeof(kto), kbody, sizeof(kbody));

    nvs_handle_t h;
    if (!msgq_open(NVS_READWRITE, &h)) {
        return;
    }
    nvs_erase_key(h, kid);
    nvs_erase_key(h, kto);
    nvs_erase_key(h, kbody);
    nvs_commit(h);
    nvs_close(h);
}

// Used both by msg_pump()'s rare RAM-ring-eviction fallback and by
// msg_init()'s recovery paths.
static bool msgq_read_reply(int slot, char *id, size_t id_cap, char *to, size_t to_cap,
                             char *body, size_t body_cap, uint16_t *body_len)
{
    char kid[8], kto[8], kbody[8];
    reply_keys(slot, kid, sizeof(kid), kto, sizeof(kto), kbody, sizeof(kbody));

    nvs_handle_t h;
    if (!msgq_open(NVS_READONLY, &h)) {
        return false;
    }
    size_t len = id_cap;
    bool ok = nvs_get_str(h, kid, id, &len) == ESP_OK;
    if (ok) {
        len = to_cap;
        ok = nvs_get_str(h, kto, to, &len) == ESP_OK;
    }
    if (ok) {
        len = body_cap;
        ok = nvs_get_str(h, kbody, body, &len) == ESP_OK;
    }
    nvs_close(h);
    if (ok && body_len) {
        *body_len = (uint16_t) strlen(body);
    }
    return ok;
}

static bool msgq_write_unread(const char *id, const char *from, int64_t ts, const char *body)
{
    nvs_handle_t h;
    if (!msgq_open(NVS_READWRITE, &h)) {
        return false;
    }
    bool ok = nvs_set_str(h, "uid", id) == ESP_OK && nvs_set_str(h, "ufrom", from) == ESP_OK &&
              nvs_set_i64(h, "uts", ts) == ESP_OK && nvs_set_str(h, "ubody", body) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static void msgq_erase_unread(void)
{
    nvs_handle_t h;
    if (!msgq_open(NVS_READWRITE, &h)) {
        return;
    }
    nvs_erase_key(h, "uid");
    nvs_erase_key(h, "ufrom");
    nvs_erase_key(h, "uts");
    nvs_erase_key(h, "ubody");
    nvs_commit(h);
    nvs_close(h);
}

static bool msgq_read_unread(char *id, size_t id_cap, char *from, size_t from_cap, int64_t *ts,
                              char *body, size_t body_cap, uint16_t *body_len)
{
    nvs_handle_t h;
    if (!msgq_open(NVS_READONLY, &h)) {
        return false;
    }
    size_t len = id_cap;
    bool ok = nvs_get_str(h, "uid", id, &len) == ESP_OK;
    if (ok) {
        len = from_cap;
        ok = nvs_get_str(h, "ufrom", from, &len) == ESP_OK;
    }
    if (ok) {
        ok = nvs_get_i64(h, "uts", ts) == ESP_OK;
    }
    if (ok) {
        len = body_cap;
        ok = nvs_get_str(h, "ubody", body, &len) == ESP_OK;
    }
    nvs_close(h);
    if (ok && body_len) {
        *body_len = (uint16_t) strlen(body);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// msg_init and its NVS-backed recovery paths (docs/PROTOCOL.md §9.2's
// durability claim: a reply/unread body now outlives a battery pull, not
// just a warm reset, because it lives in NVS rather than RTC).
// ---------------------------------------------------------------------------

// Warm reset (RTC survived): RTC's pending_up metadata survived but s_thread
// (.bss) did not — repopulate the RAM view from the NVS body so a reply
// typed just before a crash/watchdog reset is still visible. msg_pump()
// keeps retrying regardless (it reads RTC metadata directly), so this only
// affects what the UI shows, not delivery. Guarded against history_restore()
// (called first by msg_init(), below) having already restored this exact
// row from `msghist` — msg_queue_reply() persists a reply at insert time, so
// a still-pending reply is typically already sitting in s_thread by the
// time this runs, and inserting it a second time would show it twice.
static void warm_recover_replies(void)
{
    for (int i = 0; i < MSG_PENDING_UP_MAX; i++) {
        s_lock();
        bool in_use = s_rtc->pending_up[i].in_use;
        char id[MSG_ID_MAX] = "";
        char to[MSG_TO_MAX] = "";
        bool already = false;
        if (in_use) {
            strncpy(id, s_rtc->pending_up[i].id, MSG_ID_MAX - 1);
            strncpy(to, s_rtc->pending_up[i].to, MSG_TO_MAX - 1);
            already = thread_find_locked(id, MSG_DIR_UP) != NULL;
        }
        s_unlock();
        if (!in_use || already) {
            continue;
        }

        char nid[MSG_ID_MAX] = "", nto[MSG_TO_MAX] = "", body[MSG_RAM_BODY_MAX] = "";
        uint16_t body_len = 0;
        if (!msgq_read_reply(i, nid, sizeof(nid), nto, sizeof(nto), body, sizeof(body),
                              &body_len)) {
            continue; // NVS body missing/corrupt: msg_pump() still retries from RTC alone
        }

        msg_t entry = { 0 };
        strncpy(entry.id, id, MSG_ID_MAX - 1);
        strncpy(entry.to, to, MSG_TO_MAX - 1);
        strncpy(entry.from, "student", MSG_FROM_MAX - 1);
        memcpy(entry.body, body, body_len);
        entry.body[body_len] = '\0';
        entry.body_len = body_len;
        entry.dir = (uint8_t) MSG_DIR_UP;
        entry.ack_state = MSG_ACK_UP_PENDING;
        entry.flags = MSG_F_RECOVERED;
        entry.in_use = true;
        // No hist_seq assigned/history_write_entry() call here: this row
        // either already exists in msghist (msg_queue_reply() persisted it
        // at insert) or msghist is unavailable, in which case there is
        // nothing to persist to. Re-persisting a msgq-only reconstruction
        // under a *new* seq would also orphan whatever seq the original
        // insert used, wasting a ring slot for no benefit.
        s_lock();
        thread_insert_locked(&entry);
        s_unlock();
    }
}

// Warm reset: RTC's unread[0] metadata survived; body comes from NVS. Same
// history_restore()-already-has-it guard as warm_recover_replies() above.
// Returns true if there was an unread message to (attempt to) recover.
static bool warm_recover_unread(void)
{
    s_lock();
    bool had_unread = s_rtc->unread[0].in_use;
    char id[MSG_ID_MAX] = "", from[MSG_FROM_MAX] = "";
    int64_t ts = 0;
    bool already = false;
    if (had_unread) {
        strncpy(id, s_rtc->unread[0].id, MSG_ID_MAX - 1);
        strncpy(from, s_rtc->unread[0].from, MSG_FROM_MAX - 1);
        ts = s_rtc->unread[0].ts;
        already = thread_find_locked(id, MSG_DIR_DOWN) != NULL;
    }
    s_unlock();
    if (!had_unread) {
        return false;
    }
    if (already) {
        return true; // msghist already restored this row; RTC metadata is already correct
    }

    char nid[MSG_ID_MAX] = "", nfrom[MSG_FROM_MAX] = "", body[MSG_RAM_BODY_MAX] = "";
    int64_t nts = 0;
    uint16_t body_len = 0;
    if (!msgq_read_unread(nid, sizeof(nid), nfrom, sizeof(nfrom), &nts, body, sizeof(body),
                          &body_len)) {
        return true; // RTC says there should be one; NVS body missing — still "had_unread"
    }

    msg_t entry = { 0 };
    entry.ts = ts;
    strncpy(entry.id, id, MSG_ID_MAX - 1);
    strncpy(entry.from, from, MSG_FROM_MAX - 1);
    memcpy(entry.body, body, body_len);
    entry.body[body_len] = '\0';
    entry.body_len = body_len;
    entry.dir = (uint8_t) MSG_DIR_DOWN;
    entry.ack_state = MSG_ACK_SHOWN; // unread[0] only holds shown-not-read messages
    entry.flags = MSG_F_RECOVERED;
    entry.in_use = true;
    // G7: entry.sndr is left "" (entry was zero-initialised above) — msg_unread_t
    // has no sndr field (msg.h's own comment on that struct), so a warm-recovered
    // group page's author line is lost here; only its `from` (the group alias)
    // survives. Accepted: this path only runs when msghist itself is unavailable.
    s_lock();
    thread_insert_locked(&entry);
    s_unlock();
    return true;
}

// Cold boot (RTC lost): RTC metadata is zeroed, but a reply's NVS body can
// still be sitting there from before the battery pull — sweep both slots
// and rebuild the minimum RTC+RAM state to keep sending it. Age/attempts
// reset to fresh (a cold boot is a total loss-of-context event, §9.6); only
// the content itself is preserved. The RTC pending_up reconstruction below
// always runs (RTC really was wiped); the s_thread insertion is skipped
// when history_restore() (called first by msg_init()) already put this id
// in the thread, same reasoning as warm_recover_replies() above.
static void cold_recover_replies(void)
{
    for (int i = 0; i < MSG_PENDING_UP_MAX; i++) {
        char id[MSG_ID_MAX] = "", to[MSG_TO_MAX] = "", body[MSG_RAM_BODY_MAX] = "";
        uint16_t body_len = 0;
        if (!msgq_read_reply(i, id, sizeof(id), to, sizeof(to), body, sizeof(body), &body_len)) {
            continue;
        }

        s_lock();
        msg_pending_up_t *p = &s_rtc->pending_up[i];
        p->created_us = esp_timer_get_time();
        strncpy(p->id, id, MSG_ID_MAX - 1);
        strncpy(p->to, to, MSG_TO_MAX - 1);
        p->attempts = 0;
        p->in_use = true;
        s_save();

        bool already = thread_find_locked(id, MSG_DIR_UP) != NULL;
        if (!already) {
            msg_t entry = { 0 };
            strncpy(entry.id, id, MSG_ID_MAX - 1);
            strncpy(entry.to, to, MSG_TO_MAX - 1);
            strncpy(entry.from, "student", MSG_FROM_MAX - 1);
            memcpy(entry.body, body, body_len);
            entry.body[body_len] = '\0';
            entry.body_len = body_len;
            entry.dir = (uint8_t) MSG_DIR_UP;
            entry.ack_state = MSG_ACK_UP_PENDING;
            entry.flags = MSG_F_RECOVERED;
            entry.in_use = true;
            thread_insert_locked(&entry);
        }
        s_unlock();

        ESP_LOGI(TAG, "cold boot: recovered pending reply slot %d from NVS msgq (%u bytes)", i,
                 (unsigned) body_len);
    }
}

// Returns true if an orphaned unread message was found and recovered.
// Same "RTC metadata always rebuilt, s_thread insert skipped if
// history_restore() already has it" split as cold_recover_replies() above.
static bool cold_recover_unread(void)
{
    char id[MSG_ID_MAX] = "", from[MSG_FROM_MAX] = "", body[MSG_RAM_BODY_MAX] = "";
    int64_t ts = 0;
    uint16_t body_len = 0;
    if (!msgq_read_unread(id, sizeof(id), from, sizeof(from), &ts, body, sizeof(body),
                          &body_len)) {
        return false;
    }

    s_lock();
    msg_unread_t *u = &s_rtc->unread[0];
    u->ts = ts;
    strncpy(u->id, id, MSG_ID_MAX - 1);
    strncpy(u->from, from, MSG_FROM_MAX - 1);
    u->in_use = true;
    s_save();

    bool already = thread_find_locked(id, MSG_DIR_DOWN) != NULL;
    if (!already) {
        msg_t entry = { 0 };
        entry.ts = ts;
        strncpy(entry.id, id, MSG_ID_MAX - 1);
        strncpy(entry.from, from, MSG_FROM_MAX - 1);
        memcpy(entry.body, body, body_len);
        entry.body[body_len] = '\0';
        entry.body_len = body_len;
        entry.dir = (uint8_t) MSG_DIR_DOWN;
        entry.ack_state = MSG_ACK_SHOWN;
        entry.flags = MSG_F_RECOVERED;
        entry.in_use = true;
        // G7: same "sndr lost" acceptance as warm_recover_unread() above —
        // msg_unread_t carries no sndr, entry.sndr stays "" (zero-initialised).
        thread_insert_locked(&entry);
    }
    s_unlock();

    ESP_LOGI(TAG, "cold boot: recovered newest-unread message from NVS msgq (%u bytes)",
             (unsigned) body_len);
    return true;
}

void msg_init(bool rtc_was_valid)
{
    memset(s_thread, 0, sizeof(s_thread));
    msg_composer_reset();
    s_history_lost_pending = false;

    // Owner task 2026-09-20: mount `msghist` and restore whatever it has
    // BEFORE the msgq-based pending-reply/unread reconstruction below, on
    // both a cold and a warm boot — those two only ever rebuild a couple of
    // in-flight rows and actively check for (and skip duplicating) a row
    // history_restore() already placed here, see their own comments.
    history_mount();
    history_restore();

    if (!rtc_was_valid) {
        // Cold boot: RTC's own zeroing is modes.c's job (rtc_cold_init());
        // msgq NVS content is what lets an in-flight reply/unread's RTC
        // retry/ack metadata survive even though the RTC struct itself did
        // not (msghist has no opinion on retry state, only on what to draw).
        cold_recover_replies();
        cold_recover_unread();
    } else {
        // Reset recovery: RTC's own metadata survived; msgq is still the
        // source for any full-fidelity body msghist did not already supply
        // (msg.h/§9.4 — bodies never lived in RTC itself).
        warm_recover_replies();
        warm_recover_unread();
    }

    // "History really lost" (owner instruction) now means exactly what it
    // says: no stored history could be reconstructed by ANY of the paths
    // above — first boot ever, an erased/not-yet-provisioned `msghist`
    // partition, or (rare) every stored record failing its version/CRC
    // check. Any of the recovery paths above having put at least one row
    // in s_thread means there IS stored history, so the flag stays false —
    // this is the behaviour change from pre-msghist firmware, which armed
    // this flag on every single reset regardless (msg.c's own git history).
    size_t n = 0;
    for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
        if (s_thread[i].in_use) {
            n++;
        }
    }
    if (n == 0) {
        s_history_lost_pending = true;
        ESP_LOGI(TAG, "%s: no stored history found, history_lost flag armed",
                 rtc_was_valid ? "reset recovery" : "cold boot");
    } else {
        ESP_LOGI(TAG, "%s: thread has %u message(s) after recovery",
                 rtc_was_valid ? "reset recovery" : "cold boot", (unsigned) n);
    }
}

// ---------------------------------------------------------------------------
// Down-message validation (PROTOCOL.md §3.1/§3.4) and ingest.
// ---------------------------------------------------------------------------

static bool id_shape_ok(const char *s)
{
    size_t len = strlen(s);
    if (len < 3 || len > 16) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool body_rules_ok(const char *body, size_t body_len)
{
    if (body_len == 0 || body_len > 320) {
        return false;
    }
    size_t codepoints = 0;
    for (size_t i = 0; i < body_len; i++) {
        unsigned char c = (unsigned char) body[i];
        if (c < 0x20 || c == 0x7F) {
            return false; // control char, §3.1
        }
        if (!is_utf8_cont(c)) {
            codepoints++;
        }
    }
    return codepoints <= 160;
}

// Shared tail of msg_ingest_down_cbor() — dedup (§4.1 rule 7), thread
// insert, NVS/RTC unread mirror (§9.2/§9.3/§9.4). Callers have already
// validated every field; `id_str`/`from_str`/`body_str` need not be the
// caller's own storage (this function makes its own bounded copies before
// returning).
static msg_ingest_t ingest_common(const char *id_str, int64_t ts, const char *from_str,
                                   const char *sndr_str, const char *body_str, size_t body_len,
                                   const msg_t **out)
{
    uint32_t digest = id_digest(id_str);
    char id_copy[MSG_ID_MAX];
    strncpy(id_copy, id_str, MSG_ID_MAX - 1);
    id_copy[MSG_ID_MAX - 1] = '\0';
    strncpy(s_last_ingest_id, id_copy, MSG_ID_MAX - 1);
    s_last_ingest_id[MSG_ID_MAX - 1] = '\0';
    char from_copy[MSG_FROM_MAX];
    strncpy(from_copy, from_str, MSG_FROM_MAX - 1);
    from_copy[MSG_FROM_MAX - 1] = '\0';
    char sndr_copy[MSG_FROM_MAX];
    strncpy(sndr_copy, sndr_str, MSG_FROM_MAX - 1);
    sndr_copy[MSG_FROM_MAX - 1] = '\0';
    char body_copy[MSG_RAM_BODY_MAX];
    strncpy(body_copy, body_str, MSG_RAM_BODY_MAX - 1);
    body_copy[MSG_RAM_BODY_MAX - 1] = '\0';

    bool is_dup;
    s_lock();
    is_dup = seen_contains_locked(digest);
    if (!is_dup) {
        // Owner task 2026-09-20: the RTC seen_ids ring (§4.1 rule 7) is only
        // 16 deep and lost on every cold boot, but msghist's persisted
        // history — restored into s_thread by msg_init()/history_restore()
        // before this function's caller ever runs — is neither. Consulting
        // it here is what makes a message the relay re-publishes on the
        // next online edge after a cold boot (§5.3) still recognised as a
        // duplicate (re-acked below, not re-rendered) instead of shown
        // twice, closing the same gap this function's pre-existing comment
        // already describes for a same-boot duplicate whose id fell out of
        // the (narrower) seen_ids ring before this task.
        is_dup = thread_find_locked(id_copy, MSG_DIR_DOWN) != NULL;
    }
    if (!is_dup) {
        seen_add_locked(digest);
    } else {
        s_rtc->dedup_hits++;
    }
    s_save();
    s_unlock();

    if (is_dup) {
        if (out) {
            // Same brief locked-pointer-handoff pattern as before F6.4: this
            // ingest-path `out` is consumed synchronously by modes.c's
            // handle_ingest_result() on the same call stack, not one of the
            // README R6 accessors (msg_thread_at()/msg_newest_unread())
            // that copy under the lock — see msg.h's doc comment on those.
            s_lock();
            *out = thread_find_locked(id_copy, MSG_DIR_DOWN);
            s_unlock();
        }
        return MSG_INGEST_DUPLICATE;
    }

    msg_t entry = { 0 };
    entry.ts = ts;
    strncpy(entry.id, id_copy, MSG_ID_MAX - 1);
    strncpy(entry.from, from_copy, MSG_FROM_MAX - 1);
    entry.to[0] = '\0'; // down messages have no outbound `to`
    strncpy(entry.sndr, sndr_copy, MSG_FROM_MAX - 1); // G7: "" unless this is a group page
    strncpy(entry.body, body_copy, MSG_RAM_BODY_MAX - 1);
    entry.body_len = (uint16_t) body_len;
    entry.dir = (uint8_t) MSG_DIR_DOWN;
    entry.ack_state = MSG_ACK_UNSHOWN;
    entry.flags = 0;
    entry.in_use = true;
    // Owner task 2026-09-20: this is the "insert" write of msghist's
    // "at most 2 writes per message" budget (msg.h's module comment) — the
    // second, if any, happens later when this id's ack_state first reaches
    // MSG_ACK_READ (mark_common()). Assigned before thread_insert_locked()
    // so the copy that lands in s_thread[0] carries it too (mark_common()
    // needs it to know which msghist key to rewrite).
    entry.hist_seq = history_next_seq();

    // Full-fidelity NVS write BEFORE touching the RAM thread/RTC metadata,
    // and with the lock released (flash I/O; matches msg_pump()'s existing
    // no-I/O-under-lock discipline) — keeps the locked section immediately
    // below exactly as short as it was before F6.4.
    msgq_write_unread(id_copy, from_copy, ts, body_copy);

    s_lock();
    thread_insert_locked(&entry);
    if (out) {
        *out = &s_thread[0];
    }

    // RTC mirror is now metadata-only (§9.2/§9.3) — the newest ingest is
    // always "the newest unread entry" under this design's single-slot
    // mirror.
    msg_unread_t *u = &s_rtc->unread[0];
    u->ts = ts;
    strncpy(u->id, id_copy, MSG_ID_MAX - 1);
    u->id[MSG_ID_MAX - 1] = '\0';
    strncpy(u->from, from_copy, MSG_FROM_MAX - 1);
    u->from[MSG_FROM_MAX - 1] = '\0';
    u->in_use = true;
    s_save();
    s_unlock();

    history_write_entry(&entry); // unlocked flash I/O, same discipline as msgq_write_unread() above

    return MSG_INGEST_NEW;
}

// F3.6 (docs/PROTOCOL.md §14.3/§14.4), F6.4 (docs/DEVICE_PLAN.md H14): every
// device decodes CBOR; `n`/the replay window only apply when `ident`'s
// IDENT_FLAG_REQ_SIG is set (§10 keymap: key 12 `n` is "signed envelopes"
// only). See msg.h's doc comment for the full caller contract.
msg_ingest_t msg_ingest_down_cbor(const uint8_t *buf, uint16_t len, const msg_t **out)
{
    if (out) {
        *out = NULL;
    }
    s_last_ingest_id[0] = '\0';

    bool req_sig = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;

    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count) || count == 0) {
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    if (req_sig) {
        count -= 1; // the trimmed `sig` pair (key 13), see msg.h — only ever present when signed
    }

    char id_local[MSG_ID_MAX] = "";
    char from_local[MSG_FROM_MAX] = "";
    char sndr_local[MSG_FROM_MAX] = ""; // G7: absent/bad -> stays "" (author line falls back to `from`)
    char body_local[MSG_RAM_BODY_MAX] = "";
    int64_t ts = 0;
    size_t body_len = 0;
    uint32_t n = 0;
    bool have_id = false, have_ts = false, have_from = false, have_body = false;
    bool have_ack_null = false, have_n = false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            msg_count_malformed();
            return MSG_INGEST_MALFORMED;
        }
        switch (key) {
        case MK_V: {
            uint64_t v;
            if (!cbor_r_uint(&r, &v)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            break;
        }
        case MK_ID: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen == 0 || slen >= sizeof(id_local)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            memcpy(id_local, s, slen);
            id_local[slen] = '\0';
            if (!id_shape_ok(id_local)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            have_id = true;
            break;
        }
        case MK_TS: {
            int64_t v;
            if (cbor_r_peek(&r) == CBOR_T_NINT) {
                if (!cbor_r_nint(&r, &v)) {
                    msg_count_malformed();
                    return MSG_INGEST_MALFORMED;
                }
            } else {
                uint64_t u;
                if (!cbor_r_uint(&r, &u)) {
                    msg_count_malformed();
                    return MSG_INGEST_MALFORMED;
                }
                v = (int64_t) u;
            }
            if (!(v == 0 || (v >= 1000000000LL && v <= 2000000000LL))) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            ts = v;
            have_ts = true;
            break;
        }
        case MK_FROM: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen == 0 || slen >= sizeof(from_local)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            memcpy(from_local, s, slen);
            from_local[slen] = '\0';
            have_from = true;
            break;
        }
        case MK_SNDR: {
            // G7 (docs/GROUP_CHAT_DESIGN.md §4): absent is normal (every DM
            // and every pre-G7 page), bad is ignored, never malformed —
            // length-only, exactly the rule MK_FROM gets above, minus the
            // "must be present" requirement. Only a truncated/unreadable
            // CBOR head (cbor_r_skip() also failing) is malformed, which is
            // already true of every other key.
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) { // wrong CBOR type (e.g. a uint): ignore it
                if (!cbor_r_skip(&r)) {
                    msg_count_malformed();
                    return MSG_INGEST_MALFORMED;
                }
                break;
            }
            if (slen > 0 && slen < sizeof(sndr_local)) {
                memcpy(sndr_local, s, slen);
                sndr_local[slen] = '\0';
            }
            break; // empty or >16 chars: stays ""
        }
        case MK_BODY: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(body_local)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            memcpy(body_local, s, slen);
            body_local[slen] = '\0';
            if (!body_rules_ok(body_local, slen)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            body_len = slen;
            have_body = true;
            break;
        }
        case MK_ACK: {
            // Device only ever receives content messages on /down (§3.2):
            // a non-null ack here is malformed.
            if (cbor_r_peek(&r) != CBOR_T_NULL || !cbor_r_null(&r)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            have_ack_null = true;
            break;
        }
        case MK_KIND: {
            // F7.1 (docs/PROTOCOL.md §3.2): `/down` carries msg/loc_req/
            // book/cfg. `book`/`cfg` are intercepted by book.c/lock.c
            // before this function ever runs (modes.c's
            // on_incoming_message()), and a `loc_req` has no `body` field
            // so it already fails the have_body check below on its own —
            // this explicit check is defense in depth against any *other*
            // non-"msg" kind value reaching here (e.g. a future relay bug
            // that attaches a body to something that is not a content
            // message). Absent `kind` still means "msg" (§3.2's own
            // default), so no case at all is the common, unsigned-cost path.
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            if (!(slen == 3 && memcmp(s, "msg", 3) == 0)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            break;
        }
        case MK_N: {
            // v0.2 (docs/V02_DESIGN.md §3, §7): `n` is now up to 2^53-1 on
            // the wire ("uint, now up to 2^53-1"). The down path's replay
            // window (auth_accept_down_n(), auth_rtc_t.down_n) stays a
            // uint32_t by design -- the relay's own downN counts by one, so
            // reaching 2^32 is not a realistic device lifetime -- but this
            // parser MUST still *parse* a wider value without rejecting the
            // whole envelope as malformed (a v0.1 firmware bug: rejecting
            // here previously meant a relay that ever legitimately sent a
            // wide `n` would get every /down silently dropped). Saturate
            // instead of reject; a value that actually reaches UINT32_MAX
            // fails the replay window on its own merits, the same way any
            // other implausible `n` would.
            uint64_t v;
            if (!cbor_r_uint(&r, &v)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            n = (v > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t) v;
            have_n = true;
            break;
        }
        default:
            if (!cbor_r_skip(&r)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            break;
        }
    }

    if (!have_id || !have_ts || !have_from || !have_body || !have_ack_null) {
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }

    if (req_sig) {
        // §2.5/§14.2: device-side mirror of the relay's replay window on the
        // /down counter — `n` only exists on signed envelopes (§10 keymap).
        if (!have_n || !s_auth_rtc) {
            msg_count_malformed();
            return MSG_INGEST_MALFORMED;
        }
        bool accepted;
        s_lock();
        accepted = auth_accept_down_n(s_auth_rtc, n);
        s_save();
        s_unlock();
        if (!accepted) {
            msg_count_malformed();
            ESP_LOGD(TAG, "down n=%u rejected by replay window (§2.5)", (unsigned) n);
            return MSG_INGEST_MALFORMED;
        }
    }

    return ingest_common(id_local, ts, from_local, sndr_local, body_local, body_len, out);
}

// ---------------------------------------------------------------------------
// Ack queue (§4.1 rule 6)
// ---------------------------------------------------------------------------

// Upserts a pending_ack entry for `id` at `state`. Caller holds the lock.
static bool pending_ack_upsert_locked(const char *id, uint8_t state)
{
    int free_slot = -1;
    for (int i = 0; i < MSG_PENDING_ACKS_MAX; i++) {
        if (s_rtc->pending_acks[i].in_use &&
            strncmp(s_rtc->pending_acks[i].id, id, MSG_ID_MAX) == 0) {
            s_rtc->pending_acks[i].state = state;
            s_rtc->pending_acks[i].attempts = 0;
            return true;
        }
        if (free_slot < 0 && !s_rtc->pending_acks[i].in_use) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return false; // table full
    }
    strncpy(s_rtc->pending_acks[free_slot].id, id, MSG_ID_MAX - 1);
    s_rtc->pending_acks[free_slot].id[MSG_ID_MAX - 1] = '\0';
    s_rtc->pending_acks[free_slot].state = state;
    s_rtc->pending_acks[free_slot].attempts = 0;
    s_rtc->pending_acks[free_slot].in_use = true;
    return true;
}

static bool mark_common(const char *id, uint8_t ack_state, uint8_t pending_state)
{
    s_lock();
    msg_t *m = thread_find_locked(id, MSG_DIR_DOWN);
    // v0.2 bug fix #1 (docs/V02_DESIGN.md §2.1): only advance the RAM
    // ack_state when the ack actually got queued. The old code bumped
    // ack_state unconditionally, so a message that lost the race for the
    // MSG_PENDING_ACKS_MAX (8) deep queue during a burst was marked SHOWN/READ
    // locally forever with its `shown`/`read` ack silently dropped -- a later
    // msg_mark_all_unshown() pass (or a re-render) would never retry it,
    // because it no longer looked UNSHOWN. Leaving ack_state where it was
    // when queuing fails means the next successful pass sees it as still
    // outstanding and retries. No power/modem effect: RAM bookkeeping only.
    uint8_t old_ack_state = m ? m->ack_state : 0;
    bool queued = pending_ack_upsert_locked(id, pending_state);
    if (queued && m && m->ack_state < ack_state) {
        m->ack_state = ack_state;
    }
    bool clear_unread = false;
    if (queued && ack_state == MSG_ACK_READ && s_rtc->unread[0].in_use &&
        strncmp(s_rtc->unread[0].id, id, MSG_ID_MAX) == 0) {
        s_rtc->unread[0].in_use = false;
        clear_unread = true;
    }
    // Owner task 2026-09-20: the "terminal ack_state" second msghist write
    // (msg.h's write-policy comment) — fires exactly once per message, the
    // call where ack_state actually transitions (old_ack_state != the new
    // value) into MSG_ACK_READ, never on a re-ack of an already-READ
    // message (msg_mark_all_unshown()'s own retry sweep, or a duplicate
    // ingest's re-ack, would otherwise call this repeatedly). A snapshot is
    // taken now (still locked) so the flash write below can happen after
    // s_unlock(), same discipline as msgq_erase_unread() already uses here.
    bool need_persist = false;
    msg_t snapshot = { 0 };
    if (queued && m && old_ack_state != m->ack_state &&
        msghist_is_terminal_ack(MSG_DIR_DOWN, m->ack_state)) {
        snapshot = *m;
        need_persist = true;
    }
    s_save();
    s_unlock();

    if (clear_unread) {
        msgq_erase_unread(); // unlocked flash I/O, §9.4
    }
    if (need_persist) {
        history_write_entry(&snapshot);
    }
    return queued;
}

bool msg_mark_shown(const char *id) { return mark_common(id, MSG_ACK_SHOWN, 0); }
bool msg_mark_read(const char *id) { return mark_common(id, MSG_ACK_READ, 1); }

// F6.5 (docs/DEVICE_PLAN.md §5.8) — see msg.h's own doc comment.
void msg_mark_all_unshown(void)
{
    size_t n = msg_thread_count();
    for (size_t i = 0; i < n; i++) {
        const msg_t *m = msg_thread_at(i);
        if (!m || m->dir != (uint8_t) MSG_DIR_DOWN || m->ack_state != MSG_ACK_UNSHOWN) {
            continue;
        }
        char id[MSG_ID_MAX];
        strncpy(id, m->id, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
        msg_mark_shown(id);
    }
}

// ---------------------------------------------------------------------------
// Reply queue (§4.2)
// ---------------------------------------------------------------------------

bool msg_queue_reply(const char *to, const char *body, uint16_t len)
{
    if (!body || !body_rules_ok(body, len)) {
        // Reject, never truncate (§9.4).
        s_lock();
        s_rtc->reply_failed++;
        s_save();
        s_unlock();
        return false;
    }

    char id[MSG_ID_MAX];
    snprintf(id, sizeof(id), "u_%08x", (unsigned) esp_random());

    // F7.3: copied defensively once, up front — see msg.h's own doc comment
    // on why this is not re-validated against the book here.
    char to_copy[MSG_TO_MAX];
    strncpy(to_copy, to ? to : "", MSG_TO_MAX - 1);
    to_copy[MSG_TO_MAX - 1] = '\0';

    s_lock();
    int slot = -1;
    for (int i = 0; i < MSG_PENDING_UP_MAX; i++) {
        if (!s_rtc->pending_up[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        s_rtc->reply_failed++;
        s_save();
        s_unlock();
        return false;
    }
    // Reserve the slot immediately, before the unlocked NVS write below, so
    // a second msg_queue_reply() call can't race onto the same slot.
    s_rtc->pending_up[slot].in_use = true;
    s_save();
    s_unlock();

    // NVS write with the lock released (flash I/O, §4.2/§9.4) — matches
    // msg_pump()'s existing discipline of never blocking under the lock.
    bool wrote = msgq_write_reply(slot, id, to_copy, body);
    // Owner task 2026-09-20: allocated here, NOT inside the s_lock() block
    // below — history_next_seq() takes msg.c's own lock itself
    // (xSemaphoreCreateMutexStatic in modes.c is not recursive), so calling
    // it while already holding that lock would deadlock.
    uint32_t hist_seq = history_next_seq();

    s_lock();
    if (!wrote) {
        s_rtc->pending_up[slot].in_use = false; // release the reservation
        s_rtc->reply_failed++;
        s_save();
        s_unlock();
        return false;
    }

    msg_pending_up_t *p = &s_rtc->pending_up[slot];
    p->created_us = esp_timer_get_time();
    strncpy(p->id, id, MSG_ID_MAX - 1);
    p->id[MSG_ID_MAX - 1] = '\0';
    strncpy(p->to, to_copy, MSG_TO_MAX - 1);
    p->to[MSG_TO_MAX - 1] = '\0';
    p->attempts = 0;
    s_save();

    msg_t entry = { 0 };
    entry.ts = 0; // filled in at publish time from the network clock (§3.5)
    strncpy(entry.id, id, MSG_ID_MAX - 1);
    strncpy(entry.from, "student", MSG_FROM_MAX - 1);
    strncpy(entry.to, to_copy, MSG_TO_MAX - 1);
    entry.to[MSG_TO_MAX - 1] = '\0';
    memcpy(entry.body, body, len);
    entry.body[len] = '\0';
    entry.body_len = len;
    entry.dir = (uint8_t) MSG_DIR_UP;
    entry.ack_state = MSG_ACK_UP_PENDING;
    entry.flags = 0;
    entry.in_use = true;
    entry.hist_seq = hist_seq; // insert-time msghist write, see the allocation comment above
    thread_insert_locked(&entry);
    s_unlock();

    history_write_entry(&entry); // unlocked flash I/O, same discipline as msgq_write_reply() above

    return true;
}

// v0.2 §6 (docs/V02_DESIGN.md, sms.c): "x_" is a synthetic, non-wire id
// namespace distinct from every id this codebase actually publishes ("u_"
// replies, "l_" /loc, "s_" sms_log audit, "m_"/"b_" relay-issued down ids) —
// see msg.h's own doc comments on msg_insert_sms_in()/
// msg_insert_sms_out_pending() for why that separation matters (these ids
// are never shown/read-acked, never published, purely a thread-row handle).
static void gen_local_id(char *out, size_t cap)
{
    snprintf(out, cap, "x_%08x", (unsigned) esp_random());
}

bool msg_insert_sms_in(const char *from, const char *body, uint16_t body_len, int64_t ts,
                       char *out_id, size_t out_id_cap)
{
    if (!body || !body_rules_ok(body, body_len)) {
        return false;
    }
    char id[MSG_ID_MAX];
    gen_local_id(id, sizeof(id));

    msg_t entry = { 0 };
    entry.ts = ts; // S1: caller's net_get_clock() result; 0 = no clock yet (§3.5), never a new sentinel
    strncpy(entry.id, id, MSG_ID_MAX - 1);
    strncpy(entry.from, from ? from : "", MSG_FROM_MAX - 1);
    entry.to[0] = '\0';
    memcpy(entry.body, body, body_len);
    entry.body[body_len] = '\0';
    entry.body_len = body_len;
    entry.dir = (uint8_t) MSG_DIR_DOWN;
    // MSG_ACK_READ from the start (not MSG_ACK_UNSHOWN then advanced) is
    // what keeps msg_mark_all_unshown()'s burst-ack sweep from ever queuing
    // a pending_ack for this id — see msg.h's own doc comment.
    entry.ack_state = MSG_ACK_READ;
    entry.flags = 0;
    entry.in_use = true;
    // Already at its terminal ack_state (MSG_ACK_READ, never advanced) —
    // this is the ONE msghist write this row will ever get, unlike a
    // down/`msg` row's separate insert+terminal writes.
    entry.hist_seq = history_next_seq();

    s_lock();
    thread_insert_locked(&entry);
    s_unlock();

    history_write_entry(&entry); // unlocked flash I/O, see msg_queue_reply()'s own comment

    if (out_id && out_id_cap > 0) {
        strncpy(out_id, id, out_id_cap - 1);
        out_id[out_id_cap - 1] = '\0';
    }
    return true;
}

bool msg_insert_sms_out_pending(const char *to, const char *body, uint16_t body_len, char *out_id,
                                size_t out_id_cap)
{
    if (!body || !body_rules_ok(body, body_len)) {
        return false;
    }
    char id[MSG_ID_MAX];
    gen_local_id(id, sizeof(id));

    msg_t entry = { 0 };
    entry.ts = 0;
    strncpy(entry.id, id, MSG_ID_MAX - 1);
    strncpy(entry.from, "student", MSG_FROM_MAX - 1);
    strncpy(entry.to, to ? to : "", MSG_TO_MAX - 1);
    entry.to[MSG_TO_MAX - 1] = '\0';
    memcpy(entry.body, body, body_len);
    entry.body[body_len] = '\0';
    entry.body_len = body_len;
    entry.dir = (uint8_t) MSG_DIR_UP;
    entry.ack_state = MSG_ACK_UP_PENDING;
    entry.flags = 0;
    entry.in_use = true;
    entry.hist_seq = history_next_seq(); // insert write; msg_finish_sms_out() below does the terminal one

    s_lock();
    thread_insert_locked(&entry);
    s_unlock();

    history_write_entry(&entry); // unlocked flash I/O, see msg_queue_reply()'s own comment

    if (out_id && out_id_cap > 0) {
        strncpy(out_id, id, out_id_cap - 1);
        out_id[out_id_cap - 1] = '\0';
    }
    return true;
}

bool msg_finish_sms_out(const char *id, bool ok)
{
    if (!id || id[0] == '\0') {
        return false;
    }
    s_lock();
    msg_t *m = thread_find_locked(id, MSG_DIR_UP);
    bool need_persist = false;
    msg_t snapshot = { 0 };
    if (m) {
        uint8_t old_ack_state = m->ack_state;
        if (ok) {
            m->ack_state = MSG_ACK_UP_SENT;
        } else {
            m->ack_state = MSG_ACK_UP_FAILED;
            m->flags |= MSG_F_SEND_FAILED;
        }
        // Terminal-state msghist write (msg.h's write-policy comment) —
        // MSG_ACK_UP_SENT/MSG_ACK_UP_FAILED are always terminal for an up
        // message, so any real transition here is worth persisting once.
        if (old_ack_state != m->ack_state) {
            snapshot = *m;
            need_persist = true;
        }
    }
    s_unlock();
    if (need_persist) {
        history_write_entry(&snapshot);
    }
    return m != NULL;
}

// ---------------------------------------------------------------------------
// Pump (§4.1 rule 6, §4.2). At most one publish per call.
// ---------------------------------------------------------------------------

// F3.6: allocates the next /up,/status,/loc counter value under the bound
// auth_rtc_t's lock (docs/PROTOCOL.md §14.2) and reports whether `up_lo`
// just wrapped — caller must then invoke s_on_epoch_wrap() *outside* any
// lock (it does an NVS write via ident_store(), modes.c's on_auth_epoch_
// wrap()). Returns 0 / *wrapped=false if auth was never bound (defensive;
// modes_boot() always calls msg_bind_auth() before net is up).
static uint64_t next_up_n_locked(bool *wrapped)
{
    if (wrapped) {
        *wrapped = false;
    }
    if (!s_auth_rtc) {
        return 0;
    }
    s_lock();
    uint64_t n = auth_next_up_n(s_auth_rtc, ident_get_n_epoch(), wrapped);
    s_save();
    s_unlock();
    return n;
}

// F3.6 (docs/PROTOCOL.md §2.4/§10/§14): every publish is now CBOR, signed
// with auth_sign() when ident's IDENT_FLAG_REQ_SIG is set. No modem or
// sleep-state effect beyond the one net_publish_raw() call already had.
static bool publish_ack(const char *id, const char *ack_str)
{
    int64_t ts = 0;
    net_get_clock(&ts); // best-effort; leaves ts=0 on failure per §3.5

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    uint32_t nfields = 4; // v, id, ts, ack
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by auth_sign(),
                       // never written via cbor_w_* — see the MK_* comment)
    }

    uint8_t buf[128];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, nfields);
    cbor_w_uint(&w, MK_V, 1);
    cbor_w_tstr(&w, MK_ID, id, strlen(id));
    cbor_w_uint(&w, MK_TS, (uint64_t) ts);
    cbor_w_tstr(&w, MK_ACK, ack_str, strlen(ack_str));

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());

    if (!signed_env) {
        if (w.err) {
            return false;
        }
        return net_publish_raw(topic, buf, (uint16_t) w.len, 1);
    }

    bool wrapped = false;
    uint64_t n = next_up_n_locked(&wrapped);
    cbor_w_uint(&w, MK_N, n);
    if (w.err) {
        return false;
    }
    size_t len = w.len;
    if (!auth_sign(topic, buf, &len, sizeof(buf))) {
        return false;
    }
    if (wrapped && s_on_epoch_wrap) {
        s_on_epoch_wrap();
    }
    return net_publish_raw(topic, buf, (uint16_t) len, 1);
}

static bool publish_reply(const char *id, const char *to, const char *body, uint16_t body_len)
{
    int64_t ts = 0;
    net_get_clock(&ts);

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    bool has_to = (to[0] != '\0');
    uint32_t nfields = 6; // v, id, ts, from, body, ack(null)
    if (has_to) {
        nfields += 1; // to (§5.6; F7.3 is the first real caller)
    }
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by auth_sign())
    }

    uint8_t buf[384]; // 320-byte body + envelope overhead, §9.4/§9.1
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, nfields);
    cbor_w_uint(&w, MK_V, 1);
    cbor_w_tstr(&w, MK_ID, id, strlen(id));
    cbor_w_uint(&w, MK_TS, (uint64_t) ts);
    cbor_w_tstr(&w, MK_FROM, "student", 7);
    cbor_w_tstr(&w, MK_BODY, body, body_len);
    cbor_w_null(&w, MK_ACK);
    if (has_to) {
        cbor_w_tstr(&w, MK_TO, to, strlen(to));
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());

    if (!signed_env) {
        if (w.err) {
            return false;
        }
        return net_publish_raw(topic, buf, (uint16_t) w.len, 1);
    }

    bool wrapped = false;
    uint64_t n = next_up_n_locked(&wrapped);
    cbor_w_uint(&w, MK_N, n);
    if (w.err) {
        return false;
    }
    size_t len = w.len;
    if (!auth_sign(topic, buf, &len, sizeof(buf))) {
        return false;
    }
    if (wrapped && s_on_epoch_wrap) {
        s_on_epoch_wrap();
    }
    return net_publish_raw(topic, buf, (uint16_t) len, 1);
}

void msg_pump(void)
{
    // NOTE (documented simplification, see final report): net_publish_raw()
    // returning true means WalterModem accepted the mqttPublish() call, not
    // that a PUBACK was observed — net.cpp's PUBLISHED handler does not yet
    // map a mid back to a pending_ack/pending_up entry (its own comment
    // defers that wiring to this file). Treating a successful
    // net_publish_raw() as "sent" is a known gap, not a silent one; true
    // QoS1 confirmation is future work requiring net.cpp to route
    // WALTER_MODEM_MQTT_EVENT_PUBLISHED back here by mid.

    s_lock();

    // Owner request (bench, 2026-09-21): a typed reply must not wait behind
    // the pending_acks loop below — a reply publish used to be reachable
    // only after every pending ack drained (§4.1's "one publish per call"
    // discipline gave acks first refusal every time), which combined with
    // the old msg_pump() gating (modes.c's pump_blocked/skip_sleep coupling,
    // now removed — see modes.c's pump_blocked doc comment) to sit a reply
    // unsent for 116s on the bench. Trying pending_up first instead does not
    // starve acks: MSG_PENDING_UP_MAX is 2, so this loop finds nothing to do
    // (falls through without an early return) on almost every call, and even
    // a full replies queue only delays an ack by at most 2 publishes now
    // that modes.c calls msg_pump() every ~100ms-5s instead of once per
    // wake-and-drain cycle.
    for (int i = 0; i < MSG_PENDING_UP_MAX; i++) {
        if (!s_rtc->pending_up[i].in_use) {
            continue;
        }
        msg_pending_up_t *p = &s_rtc->pending_up[i];
        bool too_old = (esp_timer_get_time() - p->created_us) > (int64_t) 2 * 3600 * 1000000;
        char id_copy[MSG_ID_MAX];
        strncpy(id_copy, p->id, MSG_ID_MAX - 1);
        id_copy[MSG_ID_MAX - 1] = '\0';
        char to_copy[MSG_TO_MAX];
        strncpy(to_copy, p->to, MSG_TO_MAX - 1);
        to_copy[MSG_TO_MAX - 1] = '\0';

        // Body comes from the RAM thread (already holds full fidelity from
        // msg_queue_reply()'s own insert) rather than a fresh NVS read on
        // every retry; NVS is the fallback for the rare case a 32-deep RAM
        // ring got fully evicted before this reply was sent (§9.4/§9.5).
        char body_copy[MSG_RAM_BODY_MAX] = "";
        uint16_t body_len = 0;
        msg_t *tm = thread_find_locked(id_copy, MSG_DIR_UP);
        if (tm) {
            memcpy(body_copy, tm->body, tm->body_len);
            body_copy[tm->body_len] = '\0';
            body_len = tm->body_len;
        }
        s_unlock();

        if (!tm) {
            char nid[MSG_ID_MAX] = "", nto[MSG_TO_MAX] = "";
            uint16_t nvs_len = 0;
            if (msgq_read_reply(i, nid, sizeof(nid), nto, sizeof(nto), body_copy,
                                 sizeof(body_copy), &nvs_len)) {
                body_len = nvs_len;
            }
        }

        bool ok = (too_old || body_len == 0)
                      ? false
                      : publish_reply(id_copy, to_copy, body_copy, body_len);

        bool freed = false;
        // Owner task 2026-09-20: the terminal-state msghist write (msg.h's
        // write-policy comment) for a reply — MSG_ACK_UP_SENT/
        // MSG_ACK_UP_FAILED are always terminal, so whichever branch below
        // actually flips ack_state is always this row's second (and last)
        // msghist write. Snapshotted under the lock, written after
        // s_unlock(), same discipline as msgq_erase_reply() just below.
        bool need_persist = false;
        msg_t snapshot = { 0 };
        s_lock();
        if (ok) {
            p->in_use = false;
            freed = true;
            msg_t *m = thread_find_locked(id_copy, MSG_DIR_UP);
            if (m) {
                m->ack_state = MSG_ACK_UP_SENT;
                snapshot = *m;
                need_persist = true;
            }
        } else {
            p->attempts++;
            if (too_old || p->attempts >= 3) {
                p->in_use = false;
                freed = true;
                msg_t *m = thread_find_locked(id_copy, MSG_DIR_UP);
                if (m) {
                    m->ack_state = MSG_ACK_UP_FAILED;
                    m->flags |= MSG_F_SEND_FAILED;
                    snapshot = *m;
                    need_persist = true;
                }
            }
        }
        s_save();
        s_unlock();

        if (freed) {
            msgq_erase_reply(i); // unlocked flash I/O, §9.4
        }
        if (need_persist) {
            history_write_entry(&snapshot);
        }
        return;
    }

    for (int i = 0; i < MSG_PENDING_ACKS_MAX; i++) {
        if (!s_rtc->pending_acks[i].in_use) {
            continue;
        }
        msg_pending_ack_t *a = &s_rtc->pending_acks[i];
        const char *ack_str = (a->state == 0) ? "shown" : "read";
        char id_copy[MSG_ID_MAX];
        strncpy(id_copy, a->id, MSG_ID_MAX - 1);
        id_copy[MSG_ID_MAX - 1] = '\0';
        uint8_t state = a->state;
        s_unlock();

        bool ok = publish_ack(id_copy, ack_str);

        s_lock();
        if (ok) {
            a->in_use = false;
        } else {
            a->attempts++;
            if (a->attempts >= 3) {
                a->in_use = false; // §4.1 rule 6: drop after 3 attempts
            }
        }
        (void) state;
        s_save();
        s_unlock();
        return;
    }

    s_unlock();

    // v0.2 §6 (device-direct SMS, sms.c): reached only when neither a
    // pending reply nor a pending ack had anything to do this cycle — the
    // "one publish per call" discipline this whole function implements,
    // extended to the sms_log audit queue rather than adding a second,
    // independent publisher (V02_DESIGN.md §6's own instruction). Order
    // above is reply-then-ack (see the pending_up loop's doc comment); this
    // tail is unaffected either way, reached only once both are empty.
    sms_try_publish_one();
}

// ---------------------------------------------------------------------------
// Read-only accessors
// ---------------------------------------------------------------------------

size_t msg_thread_count(void)
{
    size_t n = 0;
    s_lock();
    for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
        if (!s_thread[i].in_use) {
            break;
        }
        n++;
    }
    s_unlock();
    return n;
}

const msg_t *msg_thread_at(size_t index)
{
    if (index >= MSG_THREAD_DEPTH) {
        return NULL;
    }
    s_lock();
    bool in_use = s_thread[index].in_use;
    if (in_use) {
        s_thread_at_snapshot = s_thread[index];
    }
    s_unlock();
    return in_use ? &s_thread_at_snapshot : NULL;
}

const msg_t *msg_newest_unread(void)
{
    bool found = false;
    s_lock();
    for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
        if (s_thread[i].in_use && s_thread[i].dir == (uint8_t) MSG_DIR_DOWN &&
            s_thread[i].ack_state != MSG_ACK_READ) {
            s_unread_snapshot = s_thread[i];
            found = true;
            break;
        }
    }
    s_unlock();
    return found ? &s_unread_snapshot : NULL;
}

static bool entry_belongs_to_peer_locked(const msg_t *m, const char *alias)
{
    if (m->dir == (uint8_t) MSG_DIR_DOWN) {
        return strncmp(m->from, alias, MSG_FROM_MAX) == 0;
    }
    return strncmp(m->to, alias, MSG_TO_MAX) == 0;
}

void msg_iter_peer(const char *alias, bool from_newest, msg_iter_peer_cb cb, void *ctx)
{
    if (!alias || !cb) {
        return;
    }
    s_lock();
    if (from_newest) {
        for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
            if (!s_thread[i].in_use) {
                break;
            }
            if (entry_belongs_to_peer_locked(&s_thread[i], alias)) {
                msg_t copy = s_thread[i];
                cb(&copy, ctx);
            }
        }
    } else {
        int last = -1;
        for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
            if (!s_thread[i].in_use) {
                break;
            }
            last = i;
        }
        for (int i = last; i >= 0; i--) {
            if (entry_belongs_to_peer_locked(&s_thread[i], alias)) {
                msg_t copy = s_thread[i];
                cb(&copy, ctx);
            }
        }
    }
    s_unlock();
}


#endif /* ESP_PLATFORM */

// msg.c — message storage, dedup, ack state machine, retry pump.
//
// Authority: docs/PROTOCOL.md §9 (storage contract), §4 (ack state machine),
// §3.1 (body limits). See msg.h for the ownership/locking discipline and
// the deliberate created_us (monotonic) deviation from the doc's literal
// "created_epoch" field name.
//
// All power-effect comments are PENDING_HW.

#include "msg.h"
#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

// F3.6 (docs/PROTOCOL.md §14, §10 keymap): signing/verification and the CBOR
// codec every publish now goes through. ident.h is needed only for the
// read-only IDENT_FLAG_REQ_SIG check and ident_get_n_epoch() — this file
// still does no NVS I/O of its own (see msg_bind_auth()'s header comment for
// where n_epoch persistence on wrap actually happens).
#include "auth.h"
#include "cbor.h"
#include "ident.h"

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
#define MK_N 12

// ---------------------------------------------------------------------------
// RTC wiring (msg.h: msg_bind_rtc()).
// ---------------------------------------------------------------------------

static msg_rtc_t *s_rtc = NULL;
static msg_rtc_lock_fn s_lock = NULL;
static msg_rtc_unlock_fn s_unlock = NULL;
static msg_rtc_save_fn s_save = NULL;

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

static char s_composer[MSG_COMPOSER_MAX];
static uint16_t s_composer_len = 0;

static bool s_history_lost_pending = false;
static char s_last_ingest_id[MSG_ID_MAX] = "";

const char *msg_last_ingest_id(void) { return s_last_ingest_id; }

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

// Truncate a UTF-8 string to at most `max_bytes` (including the NUL) at a
// code-point boundary, per PROTOCOL.md §9.4. Returns true if truncation
// actually happened.
static bool utf8_truncate(const char *src, size_t src_len, char *dst, size_t dst_cap,
                           uint16_t *out_len)
{
    size_t max_body = dst_cap - 1;
    if (src_len <= max_body) {
        memcpy(dst, src, src_len);
        dst[src_len] = '\0';
        *out_len = (uint16_t) src_len;
        return false;
    }
    size_t cut = max_body;
    // Back off while `cut` points into the middle of a multi-byte sequence
    // (a continuation byte has the top two bits `10`).
    while (cut > 0 && (((unsigned char) src[cut]) & 0xC0) == 0x80) {
        cut--;
    }
    memcpy(dst, src, cut);
    dst[cut] = '\0';
    *out_len = (uint16_t) cut;
    return true;
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
// msg_init
// ---------------------------------------------------------------------------

void msg_init(bool rtc_was_valid)
{
    memset(s_thread, 0, sizeof(s_thread));
    s_composer[0] = '\0';
    s_composer_len = 0;
    s_history_lost_pending = false;

    if (!rtc_was_valid) {
        return; // cold boot: RTC's own zeroing is modes.c's job (rtc_cold_init())
    }

    // Reset recovery: RAM thread is empty (fresh .bss); re-insert the
    // RTC-resident newest-unread message, if any, so the screen is not
    // blank, and flag that older scrollback is gone (PROTOCOL.md §9.6).
    s_lock();
    bool had_unread = s_rtc->unread[0].in_use;
    if (had_unread) {
        msg_t recovered = { 0 };
        recovered.ts = s_rtc->unread[0].ts;
        strncpy(recovered.id, s_rtc->unread[0].id, MSG_ID_MAX - 1);
        strncpy(recovered.from, s_rtc->unread[0].from, MSG_FROM_MAX - 1);
        strncpy(recovered.body, s_rtc->unread[0].body, MSG_RTC_BODY_MAX - 1);
        recovered.body_len = s_rtc->unread[0].body_len;
        recovered.dir = (uint8_t) MSG_DIR_DOWN;
        recovered.ack_state = MSG_ACK_SHOWN; // unread[0] only holds shown-not-read messages
        recovered.flags = s_rtc->unread[0].flags | MSG_F_RECOVERED;
        recovered.in_use = true;
        thread_insert_locked(&recovered);
    }
    s_unlock();

    s_history_lost_pending = true;
    ESP_LOGI(TAG, "reset recovery: thread reduced to %s, history_lost flag armed",
             had_unread ? "1 recovered message" : "0 messages");
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
        if ((c & 0xC0) != 0x80) {
            codepoints++;
        }
    }
    return codepoints <= 160;
}

// F3.6: shared tail of both msg_ingest_down() (JSON) and
// msg_ingest_down_cbor() — dedup (§4.1 rule 7), thread insert, RTC unread
// mirror (§9.2/§9.3). Callers have already validated every field; `id_str`/
// `from_str`/`body_str` need not be the caller's own storage (this function
// makes its own bounded copies before returning).
static msg_ingest_t ingest_common(const char *id_str, int64_t ts, const char *from_str,
                                   const char *body_str, size_t body_len, const msg_t **out)
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
    char body_copy[MSG_RAM_BODY_MAX];
    strncpy(body_copy, body_str, MSG_RAM_BODY_MAX - 1);
    body_copy[MSG_RAM_BODY_MAX - 1] = '\0';

    bool is_dup;
    s_lock();
    is_dup = seen_contains_locked(digest);
    if (!is_dup) {
        seen_add_locked(digest);
    } else {
        s_rtc->dedup_hits++;
    }
    s_save();
    s_unlock();

    if (is_dup) {
        if (out) {
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
    strncpy(entry.body, body_copy, MSG_RAM_BODY_MAX - 1);
    entry.body_len = (uint16_t) body_len;
    entry.dir = (uint8_t) MSG_DIR_DOWN;
    entry.ack_state = MSG_ACK_UNSHOWN;
    entry.flags = 0;
    entry.in_use = true;

    s_lock();
    thread_insert_locked(&entry);
    if (out) {
        *out = &s_thread[0];
    }

    // Mirror as the newest unread entry (§9.2/§9.3), truncated to RTC
    // fidelity (§9.4). The newest ingest is always "the newest unread
    // entry" under this design's single-slot unread mirror.
    msg_unread_t *u = &s_rtc->unread[0];
    u->ts = ts;
    strncpy(u->id, id_copy, MSG_ID_MAX - 1);
    u->id[MSG_ID_MAX - 1] = '\0';
    strncpy(u->from, from_copy, MSG_FROM_MAX - 1);
    u->from[MSG_FROM_MAX - 1] = '\0';
    bool truncated = utf8_truncate(body_copy, body_len, u->body, MSG_RTC_BODY_MAX, &u->body_len);
    u->flags = truncated ? MSG_F_TRUNCATED : 0;
    u->in_use = true;
    s_save();
    s_unlock();

    return MSG_INGEST_NEW;
}

msg_ingest_t msg_ingest_down(const char *json, uint16_t len, const msg_t **out)
{
    if (out) {
        *out = NULL;
    }
    s_last_ingest_id[0] = '\0';

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }

    cJSON *j_id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *j_ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    cJSON *j_from = cJSON_GetObjectItemCaseSensitive(root, "from");
    cJSON *j_body = cJSON_GetObjectItemCaseSensitive(root, "body");
    cJSON *j_ack = cJSON_GetObjectItemCaseSensitive(root, "ack");

    if (!j_id || !cJSON_IsString(j_id) || !id_shape_ok(j_id->valuestring)) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    if (!j_ts || !cJSON_IsNumber(j_ts)) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    double ts_d = j_ts->valuedouble;
    int64_t ts = (int64_t) ts_d;
    if (!(ts == 0 || (ts >= 1000000000LL && ts <= 2000000000LL))) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    if (!j_ack) {
        cJSON_Delete(root);
        msg_count_malformed(); // ack is required (even if null), §3.1
        return MSG_INGEST_MALFORMED;
    }
    // Device only ever receives content messages on /down (acks are
    // device->relay only, §3.2); a non-null ack here is malformed.
    if (!cJSON_IsNull(j_ack)) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    if (!j_from || !cJSON_IsString(j_from) || strlen(j_from->valuestring) > 16 ||
        strlen(j_from->valuestring) == 0) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    if (!j_body || !cJSON_IsString(j_body)) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    size_t body_len = strlen(j_body->valuestring);
    if (!body_rules_ok(j_body->valuestring, body_len)) {
        cJSON_Delete(root);
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }

    char id_local[MSG_ID_MAX];
    strncpy(id_local, j_id->valuestring, MSG_ID_MAX - 1);
    id_local[MSG_ID_MAX - 1] = '\0';
    char from_local[MSG_FROM_MAX];
    strncpy(from_local, j_from->valuestring, MSG_FROM_MAX - 1);
    from_local[MSG_FROM_MAX - 1] = '\0';
    char body_local[MSG_RAM_BODY_MAX];
    strncpy(body_local, j_body->valuestring, MSG_RAM_BODY_MAX - 1);
    body_local[MSG_RAM_BODY_MAX - 1] = '\0';

    cJSON_Delete(root);

    return ingest_common(id_local, ts, from_local, body_local, body_len, out);
}

// F3.6 (docs/PROTOCOL.md §10, §14.3/§14.4): CBOR twin of msg_ingest_down(),
// used only for ident IDENT_FLAG_REQ_SIG devices, only on a buffer that has
// already passed auth_verify() — see msg.h's doc comment on the
// declared-pair-count-minus-one accounting this requires.
msg_ingest_t msg_ingest_down_cbor(const uint8_t *buf, uint16_t len, const msg_t **out)
{
    if (out) {
        *out = NULL;
    }
    s_last_ingest_id[0] = '\0';

    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count) || count == 0) {
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }
    count -= 1; // the trimmed `sig` pair (key 13), see msg.h

    char id_local[MSG_ID_MAX] = "";
    char from_local[MSG_FROM_MAX] = "";
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
            // a non-null ack here is malformed, exactly like the JSON path.
            if (cbor_r_peek(&r) != CBOR_T_NULL || !cbor_r_null(&r)) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            have_ack_null = true;
            break;
        }
        case MK_N: {
            uint64_t v;
            if (!cbor_r_uint(&r, &v) || v > 0xFFFFFFFFu) {
                msg_count_malformed();
                return MSG_INGEST_MALFORMED;
            }
            n = (uint32_t) v;
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

    if (!have_id || !have_ts || !have_from || !have_body || !have_ack_null || !have_n) {
        msg_count_malformed();
        return MSG_INGEST_MALFORMED;
    }

    // §2.5/§14.2: device-side mirror of the relay's replay window on the
    // /down counter. A rejection here is the §3.4 malformed outcome: no
    // ack, no render, and (per auth_accept_down_n()'s own contract) no RTC
    // state change.
    if (!s_auth_rtc) {
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

    return ingest_common(id_local, ts, from_local, body_local, body_len, out);
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
    if (m && m->ack_state < ack_state) {
        m->ack_state = ack_state;
    }
    if (ack_state == MSG_ACK_READ && s_rtc->unread[0].in_use &&
        strncmp(s_rtc->unread[0].id, id, MSG_ID_MAX) == 0) {
        s_rtc->unread[0].in_use = false;
    }
    bool queued = pending_ack_upsert_locked(id, pending_state);
    s_save();
    s_unlock();
    return queued;
}

bool msg_mark_shown(const char *id) { return mark_common(id, MSG_ACK_SHOWN, 0); }
bool msg_mark_read(const char *id) { return mark_common(id, MSG_ACK_READ, 1); }

// ---------------------------------------------------------------------------
// Reply queue (§4.2)
// ---------------------------------------------------------------------------

bool msg_queue_reply(const char *body, uint16_t len)
{
    if (!body || len == 0 || len > 160) {
        // Reject, never truncate (§9.4). Zero-length is also rejected —
        // §3.1's "body MUST NOT be empty on a content message" applies
        // symmetrically to student replies.
        s_lock();
        s_rtc->reply_failed++;
        s_save();
        s_unlock();
        return false;
    }

    char id[MSG_ID_MAX];
    snprintf(id, sizeof(id), "u_%08x", (unsigned) esp_random());

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

    msg_pending_up_t *p = &s_rtc->pending_up[slot];
    p->created_us = esp_timer_get_time();
    strncpy(p->id, id, MSG_ID_MAX - 1);
    p->id[MSG_ID_MAX - 1] = '\0';
    memcpy(p->body, body, len);
    p->body[len] = '\0';
    p->body_len = len;
    p->attempts = 0;
    p->in_use = true;
    s_save();

    msg_t entry = { 0 };
    entry.ts = 0; // filled in at publish time from the network clock (§3.5)
    strncpy(entry.id, id, MSG_ID_MAX - 1);
    strncpy(entry.from, "student", MSG_FROM_MAX - 1);
    memcpy(entry.body, body, len);
    entry.body[len] = '\0';
    entry.body_len = len;
    entry.dir = (uint8_t) MSG_DIR_UP;
    entry.ack_state = MSG_ACK_UP_PENDING;
    entry.flags = 0;
    entry.in_use = true;
    thread_insert_locked(&entry);
    s_unlock();

    return true;
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
static uint32_t next_up_n_locked(bool *wrapped)
{
    if (wrapped) {
        *wrapped = false;
    }
    if (!s_auth_rtc) {
        return 0;
    }
    s_lock();
    uint32_t n = auth_next_up_n(s_auth_rtc, ident_get_n_epoch(), wrapped);
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
    uint32_t n = next_up_n_locked(&wrapped);
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

static bool publish_reply(const msg_pending_up_t *p)
{
    int64_t ts = 0;
    net_get_clock(&ts);

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    uint32_t nfields = 6; // v, id, ts, from, body, ack(null)
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by auth_sign())
    }

    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, nfields);
    cbor_w_uint(&w, MK_V, 1);
    cbor_w_tstr(&w, MK_ID, p->id, strlen(p->id));
    cbor_w_uint(&w, MK_TS, (uint64_t) ts);
    cbor_w_tstr(&w, MK_FROM, "student", 7);
    cbor_w_tstr(&w, MK_BODY, p->body, p->body_len);
    cbor_w_null(&w, MK_ACK);

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());

    if (!signed_env) {
        if (w.err) {
            return false;
        }
        return net_publish_raw(topic, buf, (uint16_t) w.len, 1);
    }

    bool wrapped = false;
    uint32_t n = next_up_n_locked(&wrapped);
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

    for (int i = 0; i < MSG_PENDING_UP_MAX; i++) {
        if (!s_rtc->pending_up[i].in_use) {
            continue;
        }
        msg_pending_up_t *p = &s_rtc->pending_up[i];
        bool too_old = (esp_timer_get_time() - p->created_us) > (int64_t) 2 * 3600 * 1000000;
        char id_copy[MSG_ID_MAX];
        strncpy(id_copy, p->id, MSG_ID_MAX - 1);
        id_copy[MSG_ID_MAX - 1] = '\0';
        s_unlock();

        bool ok = too_old ? false : publish_reply(p);

        s_lock();
        if (ok) {
            p->in_use = false;
            msg_t *m = thread_find_locked(id_copy, MSG_DIR_UP);
            if (m) {
                m->ack_state = MSG_ACK_UP_SENT;
            }
        } else {
            p->attempts++;
            if (too_old || p->attempts >= 3) {
                p->in_use = false;
                msg_t *m = thread_find_locked(id_copy, MSG_DIR_UP);
                if (m) {
                    m->ack_state = MSG_ACK_UP_FAILED;
                    m->flags |= MSG_F_SEND_FAILED;
                }
            }
        }
        s_save();
        s_unlock();
        return;
    }

    s_unlock();
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
    if (index >= MSG_THREAD_DEPTH || !s_thread[index].in_use) {
        return NULL;
    }
    return &s_thread[index];
}

const msg_t *msg_newest_unread(void)
{
    for (int i = 0; i < MSG_THREAD_DEPTH; i++) {
        if (s_thread[i].in_use && s_thread[i].dir == (uint8_t) MSG_DIR_DOWN &&
            s_thread[i].ack_state != MSG_ACK_READ) {
            return &s_thread[i];
        }
    }
    return NULL;
}

bool msg_history_lost(void)
{
    if (s_history_lost_pending) {
        s_history_lost_pending = false;
        return true;
    }
    return false;
}

void msg_get_stats(msg_stats_t *out)
{
    if (!out) {
        return;
    }
    s_lock();
    out->dedup_hits = s_rtc->dedup_hits;
    out->malformed_drops = s_rtc->malformed_drops;
    out->reply_failed = s_rtc->reply_failed;
    s_unlock();
}

// ---------------------------------------------------------------------------
// Composer buffer
// ---------------------------------------------------------------------------

void msg_composer_reset(void)
{
    s_composer[0] = '\0';
    s_composer_len = 0;
}

bool msg_composer_push_char(char c)
{
    // s_composer_len must stay <= 160 so the buffer always holds <=160
    // bytes + NUL (MSG_COMPOSER_MAX == 161).
    if (s_composer_len >= MSG_COMPOSER_MAX - 1) {
        return false;
    }
    s_composer[s_composer_len++] = c;
    s_composer[s_composer_len] = '\0';
    return true;
}

bool msg_composer_backspace(void)
{
    if (s_composer_len == 0) {
        return false;
    }
    s_composer[--s_composer_len] = '\0';
    return true;
}

const char *msg_composer_text(void) { return s_composer; }
uint16_t msg_composer_len(void) { return s_composer_len; }

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

static const char *TAG = "msg";

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
        goto malformed_no_json;
    }

    {
        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *j_ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
        cJSON *j_from = cJSON_GetObjectItemCaseSensitive(root, "from");
        cJSON *j_body = cJSON_GetObjectItemCaseSensitive(root, "body");
        cJSON *j_ack = cJSON_GetObjectItemCaseSensitive(root, "ack");

        if (!j_id || !cJSON_IsString(j_id) || !id_shape_ok(j_id->valuestring)) {
            cJSON_Delete(root);
            goto malformed;
        }
        if (!j_ts || !cJSON_IsNumber(j_ts)) {
            cJSON_Delete(root);
            goto malformed;
        }
        double ts_d = j_ts->valuedouble;
        int64_t ts = (int64_t) ts_d;
        if (!(ts == 0 || (ts >= 1000000000LL && ts <= 2000000000LL))) {
            cJSON_Delete(root);
            goto malformed;
        }
        if (!j_ack) {
            cJSON_Delete(root);
            goto malformed; // ack is required (even if null), §3.1
        }
        // Device only ever receives content messages on /down (acks are
        // device->relay only, §3.2); a non-null ack here is malformed.
        if (!cJSON_IsNull(j_ack)) {
            cJSON_Delete(root);
            goto malformed;
        }
        if (!j_from || !cJSON_IsString(j_from) || strlen(j_from->valuestring) > 16 ||
            strlen(j_from->valuestring) == 0) {
            cJSON_Delete(root);
            goto malformed;
        }
        if (!j_body || !cJSON_IsString(j_body)) {
            cJSON_Delete(root);
            goto malformed;
        }
        size_t body_len = strlen(j_body->valuestring);
        if (!body_rules_ok(j_body->valuestring, body_len)) {
            cJSON_Delete(root);
            goto malformed;
        }

        // Valid. Dedup (§4.1 rule 7) before touching the thread.
        uint32_t digest = id_digest(j_id->valuestring);
        char id_copy[MSG_ID_MAX];
        strncpy(id_copy, j_id->valuestring, MSG_ID_MAX - 1);
        id_copy[MSG_ID_MAX - 1] = '\0';
        strncpy(s_last_ingest_id, id_copy, MSG_ID_MAX - 1);
        s_last_ingest_id[MSG_ID_MAX - 1] = '\0';
        char from_copy[MSG_FROM_MAX];
        strncpy(from_copy, j_from->valuestring, MSG_FROM_MAX - 1);
        from_copy[MSG_FROM_MAX - 1] = '\0';
        char body_copy[MSG_RAM_BODY_MAX];
        strncpy(body_copy, j_body->valuestring, MSG_RAM_BODY_MAX - 1);
        body_copy[MSG_RAM_BODY_MAX - 1] = '\0';

        cJSON_Delete(root);

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

malformed:
    s_lock();
    s_rtc->malformed_drops++;
    s_save();
    s_unlock();
    return MSG_INGEST_MALFORMED;

malformed_no_json:
    s_lock();
    s_rtc->malformed_drops++;
    s_save();
    s_unlock();
    return MSG_INGEST_MALFORMED;
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

static bool publish_ack(const char *id, const char *ack_str)
{
    int64_t ts = 0;
    net_get_clock(&ts); // best-effort; leaves ts=0 on failure per §3.5

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "ts", (double) ts);
    cJSON_AddStringToObject(root, "ack", ack_str);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return false;
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());
    bool ok = net_publish(topic, out, (uint16_t) strlen(out), 1);
    cJSON_free(out);
    return ok;
}

static bool publish_reply(const msg_pending_up_t *p)
{
    int64_t ts = 0;
    net_get_clock(&ts);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "id", p->id);
    cJSON_AddNumberToObject(root, "ts", (double) ts);
    cJSON_AddStringToObject(root, "from", "student");
    cJSON_AddStringToObject(root, "body", p->body);
    cJSON_AddNullToObject(root, "ack");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return false;
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());
    bool ok = net_publish(topic, out, (uint16_t) strlen(out), 1);
    cJSON_free(out);
    return ok;
}

void msg_pump(void)
{
    // NOTE (documented simplification, see final report): net_publish()
    // returning true means WalterModem accepted the mqttPublish() call, not
    // that a PUBACK was observed — net.cpp's PUBLISHED handler does not yet
    // map a mid back to a pending_ack/pending_up entry (its own comment
    // defers that wiring to this file). Treating a successful net_publish()
    // as "sent" is a known gap, not a silent one; true QoS1 confirmation
    // is future work requiring net.cpp to route WALTER_MODEM_MQTT_EVENT_
    // PUBLISHED back here by mid.

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

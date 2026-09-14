/* msg.h — message storage, dedup, ack state machine, retry pump.
 *
 * Authority: docs/PROTOCOL.md §9 (storage contract), §4
 * (ack state machine), §3.1 (body limits), §11 (mode funnel).
 *
 * This module is plain C and does not depend on WalterModem types. It DOES
 * include net.h for two narrow reasons
 * explicitly: msg_pump() publishes via net_publish(), and building a wire
 * envelope needs net_get_device_id()/net_get_clock(). It never calls any
 * WalterModem API directly.
 *
 * RTC ownership (PROTOCOL.md §9.3): modes.c is the sole owner of the RTC
 * struct, its magic/crc32 pair and rtc_save(). msg.c only holds a pointer to
 * the nested msg_rtc_t sub-struct (embedded inside modes.c's pager_rtc_t)
 * plus lock/unlock/save callbacks modes.c hands over via msg_bind_rtc().
 * Every public entry point below that touches RTC-resident state takes the
 * lock, does its work without logging or blocking calls, and releases it —
 * mirroring the discipline PROTOCOL.md §9 demands for the MQTT-event-task
 * caller of msg_ingest_down(). The same lock is reused to guard the
 * RAM-resident thread (s_thread) and composer buffer below: it is the only
 * cross-task primitive in this design (msg_ingest_down can run on
 * WalterModem's _eventProcessingTask while ui.c renders from modes_run()'s
 * task), and contention is negligible at one wake per few seconds.
 *
 * Deliberate deviation from PROTOCOL.md §9.3's literal field name: the
 * pending_up "created_epoch" field is stored as a MONOTONIC microsecond
 * value from esp_timer_get_time(), not wall-clock epoch seconds. Using wall
 * clock would reintroduce exactly the "0 means no clock yet" sentinel
 * hazard identified in review, which requires
 * fixing elsewhere (docs/PROTOCOL.md §3.5: ts reads 0 with no network
 * clock). The field is only ever used for a relative ">2h old" age check
 * (msg_pump), which a monotonic clock answers correctly always; the wire
 * envelope's own `ts` field (wall clock, allowed to be 0 as documented) is
 * filled in separately at publish time from net_get_clock().
 */
#ifndef MSG_H
#define MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing constants — PROTOCOL.md §9.3 (RTC) / §9.5 (RAM).
 * --------------------------------------------------------------------- */

#define MSG_ID_MAX 17    /* 16 chars + NUL, PROTOCOL.md §1 */
#define MSG_FROM_MAX 17  /* 16 chars + NUL */

#define MSG_RTC_BODY_MAX 161 /* 160 bytes + NUL — RTC mirror fidelity, §9.4 */
#define MSG_RAM_BODY_MAX 321 /* 320 UTF-8 bytes + NUL — full fidelity, §9.5 */

#define MSG_SEEN_IDS_MAX 16     /* §4.1 rule 7 — do not shrink, PROTOCOL.md §9.7 */
#define MSG_PENDING_ACKS_MAX 8  /* §4.1 rule 6 */
#define MSG_PENDING_UP_MAX 2    /* §4.2 */
#define MSG_THREAD_DEPTH 10     /* §9.5, matches §5.3's re-publish cap */

#define MSG_COMPOSER_MAX 161 /* 160 bytes + NUL, §9.4 */

/* msg_t.flags */
#define MSG_F_TRUNCATED (1u << 0)   /* RTC mirror lost bytes at a codepoint boundary, §9.4 */
#define MSG_F_RECOVERED (1u << 1)   /* restored from RTC unread[0] after a reset, §9.6 */
#define MSG_F_SEND_FAILED (1u << 2) /* up message: 3 attempts or >2h old, §4.2 */

/* msg_t.dir */
typedef enum {
    MSG_DIR_DOWN = 0,
    MSG_DIR_UP = 1,
} msg_dir_t;

/* msg_t.ack_state — meaning depends on msg_t.dir.
 * DOWN: UNSHOWN -> SHOWN -> READ (monotonic, PROTOCOL.md §4).
 * UP:   PENDING -> SENT, or PENDING -> FAILED (MSG_F_SEND_FAILED also set). */
#define MSG_ACK_UNSHOWN 0
#define MSG_ACK_SHOWN 1
#define MSG_ACK_READ 2
#define MSG_ACK_UP_PENDING 0
#define MSG_ACK_UP_SENT 1
#define MSG_ACK_UP_FAILED 2

/* ---------------------------------------------------------------------
 * RAM-resident thread entry (msg.c's own .bss, PROTOCOL.md §9.5).
 * --------------------------------------------------------------------- */
typedef struct {
    int64_t ts;
    char id[MSG_ID_MAX];
    char from[MSG_FROM_MAX];
    char body[MSG_RAM_BODY_MAX];
    uint16_t body_len;
    uint8_t dir;       /* msg_dir_t */
    uint8_t ack_state; /* see above, depends on dir */
    uint8_t flags;
    bool in_use;
} msg_t;

/* ---------------------------------------------------------------------
 * RTC-resident sub-struct (PROTOCOL.md §9.3). Embedded by modes.c inside
 * its own pager_rtc_t as a `msg` field — this header defines the layout so
 * both modules agree on it; modes.c owns the storage and the CRC.
 * --------------------------------------------------------------------- */
typedef struct {
    char id[MSG_ID_MAX];
    uint8_t state; /* 0 = pending "shown", 1 = pending "read" */
    uint8_t attempts;
    bool in_use;
} msg_pending_ack_t;

typedef struct {
    int64_t created_us; /* esp_timer_get_time() at queue time — see header note, monotonic */
    char id[MSG_ID_MAX];
    char body[MSG_RTC_BODY_MAX]; /* full fidelity, never truncated, §9.4 */
    uint16_t body_len;
    uint8_t attempts;
    bool in_use;
} msg_pending_up_t;

typedef struct {
    int64_t ts;
    char id[MSG_ID_MAX];
    char from[MSG_FROM_MAX];
    char body[MSG_RTC_BODY_MAX]; /* truncated at a codepoint boundary, §9.4 */
    uint16_t body_len;
    uint8_t flags; /* MSG_F_TRUNCATED if the original was longer */
    bool in_use;
} msg_unread_t;

typedef struct {
    uint32_t seen_ids[MSG_SEEN_IDS_MAX]; /* digests, 0 = empty slot, §9.3 */
    uint32_t seen_head;

    msg_pending_ack_t pending_acks[MSG_PENDING_ACKS_MAX];
    msg_pending_up_t pending_up[MSG_PENDING_UP_MAX];
    msg_unread_t unread[1];

    uint32_t dedup_hits;
    uint32_t malformed_drops;
    uint32_t reply_failed;
} msg_rtc_t;

/* ---------------------------------------------------------------------
 * Wiring (modes.c calls this once, before msg_init()).
 * --------------------------------------------------------------------- */
typedef void (*msg_rtc_lock_fn)(void);
typedef void (*msg_rtc_unlock_fn)(void);
typedef void (*msg_rtc_save_fn)(void); /* must be called with the lock already held */

void msg_bind_rtc(msg_rtc_t *rtc, msg_rtc_lock_fn lock, msg_rtc_unlock_fn unlock,
                   msg_rtc_save_fn save);

/* ---------------------------------------------------------------------
 * Public API.
 * --------------------------------------------------------------------- */

/* cold boot -> zero the RAM thread; RTC's own zeroing is modes.c's job via
 * rtc_cold_init(). reset recovery (rtc_was_valid==true) -> RAM thread
 * starts empty (it's .bss, always zeroed on reset) but re-insert msg's
 * rtc-resident unread[0] (if in_use) as s_thread[0] with MSG_F_RECOVERED
 * set, so the screen isn't blank after a crash. Must be called after
 * msg_bind_rtc(). */
void msg_init(bool rtc_was_valid);

typedef enum {
    MSG_INGEST_NEW,
    MSG_INGEST_DUPLICATE,
    MSG_INGEST_MALFORMED,
} msg_ingest_t;

/* Caller contract: MSG_INGEST_NEW -> caller enters active mode, renders,
 * then calls msg_mark_shown(). MSG_INGEST_DUPLICATE -> re-ack only
 * (msg_mark_shown()/msg_mark_read() as appropriate) — caller MUST NOT
 * re-render, re-alert, or re-enter active mode (§4.1 rule 7). *out may be
 * NULL on this path if the original entry already scrolled out of the RAM
 * thread; the id string handed to msg_ingest_down() is still valid for
 * acking regardless. MSG_INGEST_MALFORMED -> count only, no ack, no
 * render, *out is always NULL. */
msg_ingest_t msg_ingest_down(const char *json, uint16_t len, const msg_t **out);

/* Most recently parsed message id from msg_ingest_down(), valid for
 * MSG_INGEST_NEW and MSG_INGEST_DUPLICATE (empty string for
 * MSG_INGEST_MALFORMED results where the id field itself did not validate).
 * Exists because a duplicate whose s_thread entry has already scrolled out
 * of the 10-deep RAM ring (which can happen in normal operation: the
 * 16-deep dedup ring is intentionally wider than the thread, §4.1 rule 7)
 * still needs to be re-acked by id (§4.1 rule 1), and *out is NULL in that
 * case. */
const char *msg_last_ingest_id(void);

/* down message: state -> shown, queues a pending_ack entry. Returns false
 * only if the pending_acks table was full and the ack could not be queued
 * (the ack_state is still updated in the RAM thread when found). */
bool msg_mark_shown(const char *id);

/* down message: state -> read, queues a pending_ack entry, clears rtc
 * unread[0] if it matches this id. */
bool msg_mark_read(const char *id);

/* Student reply. REJECTS (returns false) if len > 160 bytes — never
 * truncates (§9.4). Also rejects if pending_up is full (both slots
 * in_use). On success: generates id ("u_" + 8 lowercase hex from
 * esp_random()), inserts into pending_up AND into s_thread (dir=up,
 * ack_state=pending). */
bool msg_queue_reply(const char *body, uint16_t len);

/* Called once per wake cycle from modes_run() while the MQTT session is
 * connected. AT MOST ONE publish per call. Priority: pending acks
 * oldest-first (FIFO by array slot order — see msg.c), then pending
 * replies. Acks: 3 attempts then drop. Replies: 3 attempts OR
 * created_us > 2h old -> mark MSG_F_SEND_FAILED on the matching s_thread
 * entry and free the pending_up slot (§4.2). */
void msg_pump(void);

size_t msg_thread_count(void);
const msg_t *msg_thread_at(size_t index); /* 0 = newest; NULL if index >= msg_thread_count() */
const msg_t *msg_newest_unread(void);     /* NULL if none */

/* true once, after a reset recovery, until this is called for the first
 * time afterward — self-clearing one-shot (chosen semantics: "true once"
 * per the header comment in the phase brief; no separate ack/clear call is
 * exposed since a getter that also clears is simpler for a single UI
 * caller). Always false after a cold boot. */
bool msg_history_lost(void);

typedef struct {
    uint32_t dedup_hits;
    uint32_t malformed_drops;
    uint32_t reply_failed;
} msg_stats_t;
void msg_get_stats(msg_stats_t *out);

/* ---------------------------------------------------------------------
 * Composer buffer (owned here since msg_queue_reply() is the consumer of
 * its contents; ui.c drives it via these accessors instead of touching the
 * buffer directly, keeping msg.c the single owner of the 160-byte limit).
 * --------------------------------------------------------------------- */
void msg_composer_reset(void);
/* Returns false (and leaves the buffer unchanged) if appending `c` would
 * exceed MSG_COMPOSER_MAX-1 bytes. */
bool msg_composer_push_char(char c);
bool msg_composer_backspace(void);
const char *msg_composer_text(void);
uint16_t msg_composer_len(void);

#ifdef __cplusplus
}
#endif

#endif /* MSG_H */

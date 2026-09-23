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
 * caller of msg_ingest_down_cbor(). The same lock is reused to guard the
 * RAM-resident thread (s_thread) and composer buffer below: it is the only
 * cross-task primitive in this design (msg_ingest_down_cbor can run on
 * WalterModem's _eventProcessingTask while ui.c renders from modes_run()'s
 * task), and contention is negligible at one wake per few seconds.
 *
 * F6.4 (docs/DEVICE_PLAN.md §5.6, §5.2; docs/PROTOCOL.md §9.2-§9.4):
 * variable-length bodies (a pending reply, the newest unread down message)
 * no longer live in RTC at all — they are written to NVS namespace `msgq`
 * with full 320-byte/160-codepoint fidelity, and RTC keeps only the small
 * metadata needed to know a body exists and to retry/re-ack it (id, `to`,
 * attempts/flags, a monotonic timestamp). This is what let the composer's
 * caps grow from the old 160-byte/ASCII assumption to the full §3.1 limits
 * without blowing the RTC budget (docs/PROTOCOL.md §9.1).
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
 *
 * Second deviation, same section: PROTOCOL.md §9.3's `msg.unread[1]` row
 * also lists a `to[17]` field. A *down* message has no outbound recipient
 * of its own — only `from` (the sender) is meaningful — so this
 * implementation does not carry a `to` on msg_unread_t; nothing would ever
 * set it. (Flagged in the F6.4 report as a likely doc/table copy-paste from
 * the `pending_up` row above it, not re-litigated here since it doesn't
 * change any power/sleep/modem behaviour.)
 */
#ifndef MSG_H
#define MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth.h" /* auth_rtc_t — F3.6, see msg_bind_auth() below */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing constants — PROTOCOL.md §9.3 (RTC) / §9.5 (RAM) / §9.4 (composer).
 * --------------------------------------------------------------------- */

#define MSG_ID_MAX 17    /* 16 chars + NUL, PROTOCOL.md §1 */
#define MSG_FROM_MAX 17  /* 16 chars + NUL */
#define MSG_TO_MAX 17    /* 16 chars + NUL — peer alias for up messages, §5.6 */

#define MSG_RAM_BODY_MAX 321 /* 320 UTF-8 bytes + NUL — full fidelity, §9.5 */

#define MSG_SEEN_IDS_MAX 16     /* §4.1 rule 7 — do not shrink, PROTOCOL.md §9.7 */
#define MSG_PENDING_ACKS_MAX 8  /* §4.1 rule 6 */
#define MSG_PENDING_UP_MAX 2    /* §4.2 */
#define MSG_THREAD_DEPTH 32     /* §9.5 (F6.4: 10 -> 32, richer scrollback) */

/* Composer caps, docs/DEVICE_PLAN.md §5.2/§9.4: 160 Unicode code points
 * *and* 320 UTF-8 bytes, whichever binds first — refuse further input
 * rather than truncate. */
#define MSG_COMPOSER_MAX 321 /* 320 UTF-8 bytes + NUL */
#define MSG_COMPOSER_MAX_CODEPOINTS 160

/* msg_t.flags */
#define MSG_F_RECOVERED (1u << 1)   /* restored after a reset/cold boot, §9.6 */
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
    char to[MSG_TO_MAX]; /* peer alias for up messages; empty = default, §5.6 */
    /* G7 (docs/GROUP_CHAT_DESIGN.md §4, CBOR key 51): the group-message
     * author's alias, set only on a down `msg` belonging to a group
     * conversation ("" otherwise, which is the whole of every DM/pre-G7
     * page and the common case). `from` stays the group's own alias — the
     * thread identity and reply target — so this is purely an additional
     * per-row author label, never sent on `/up`. */
    char sndr[MSG_FROM_MAX];
    char body[MSG_RAM_BODY_MAX];
    uint16_t body_len;
    uint8_t dir;       /* msg_dir_t */
    uint8_t ack_state; /* see above, depends on dir */
    uint8_t flags;
    bool in_use;
    /* Owner task 2026-09-20 ("keep recent messages across reboots"): the
     * `msghist` NVS-partition slot/sequence this entry is persisted under,
     * 0 = never persisted (persistence unavailable, or this entry predates
     * the feature/was only ever a RECOVERED reconstruction — see msg.c's
     * history_write_entry() callers). Purely a msg.c-internal bookkeeping
     * field: nothing outside msg.c reads it, so it costs nothing to any
     * existing caller of msg_thread_at()/msg_iter_peer() and does not
     * change wire, RTC, or msgq NVS layout. Real sequence numbers start at
     * 1 (msg.c's history_next_seq()) so 0 is unambiguous. */
    uint32_t hist_seq;
} msg_t;

/* ---------------------------------------------------------------------
 * NVS-partition-resident message history (owner task 2026-09-20: "keep
 * recent messages on the device across reboots"). Deliberately NOT the
 * RTC (`pager_rtc_t`'s budget is a fixed, nearly-full 1184 bytes, modes.c's
 * own comment) and NOT the main `nvs` partition (24 kB total, already
 * carrying identity/msgq/sms/book — firmware/partitions.csv's own header
 * says not to grow it). Instead a dedicated NVS-type data partition
 * `msghist` (128 KB, firmware/partitions.csv, appended at the end of the
 * table so every existing partition keeps its offset), opened
 * independently via nvs_flash_init_partition()/nvs_open_from_partition()
 * in msg.c so a corrupt or full `msghist` can never affect any other NVS
 * namespace. Fails open: if the partition is missing (a pager whose table
 * has not been re-flashed yet) or fails to init, msg.c logs once at INFO
 * and runs exactly as it did before this feature (RAM-only thread, cleared
 * on every reset) — this feature must never be able to stop a page from
 * being received, rendered or acked.
 *
 * Ring indexing: MSG_THREAD_DEPTH (32) NVS keys, "m0".."m31" — one per
 * *sequence slot*, NOT one per s_thread ARRAY INDEX (which reshuffles on
 * every insert, thread_insert_locked()'s memmove). Each record carries its
 * own monotonically increasing `seq`, assigned at insert time; its key is
 * "m" + (seq % 32). This is what keeps a single message's flash cost to a
 * single NVS key for its whole life — msg.c never rewrites 32 keys to age
 * the ring by one slot the way it would if keys were addressed by the
 * live RAM array index instead. The ring is therefore self-describing:
 * restoring newest-first is "read all 32 keys, keep the ones that decode
 * with the current version and a good CRC, sort by `seq` descending"
 * (msg.c's history_restore(), msghist_restore_order() below), and the next
 * `seq` to hand out after a cold boot is simply (max observed seq) + 1 —
 * no separate head/count record, hence no extra write for that either.
 *
 * Write policy (bounds flash wear to about 2 writes per message over its
 * life, per the owner's instruction): a record is written once at
 * msg_insert time (whatever ack_state the entry starts at) and at most
 * ONE more time, the moment msghist_is_terminal_ack() first becomes true
 * for it (MSG_ACK_READ for a down message; MSG_ACK_UP_SENT or
 * MSG_ACK_UP_FAILED for an up message) — never on every intermediate
 * ack_state step (a down message's UNSHOWN -> SHOWN transition is never,
 * on its own, worth a second write). At an estimated 20 messages/day this
 * is at most ~40 nvs_set_blob()+nvs_commit() calls/day into a 128 KB
 * partition; NVS's own wear-levelling amortises that over many flash
 * pages long before any one sector nears a typical NOR sector's ~100k-erase
 * budget (see the final report's arithmetic).
 *
 * What a message restored in an "older" ack state does on the next boot:
 * nothing incorrect. docs/PROTOCOL.md §4.1 rule 1 — "Idempotent. A
 * repeated ack for a state already reached is a no-op (log at debug)." —
 * is exactly why this is safe: a down message persisted at its insert-time
 * UNSHOWN (because it was reset before ever being read) comes back
 * UNSHOWN, gets re-shown/re-read exactly like any other outstanding
 * message, and the relay silently drops the redundant ack instead of
 * treating it as new. Nothing here can ever *skip* a real ack or reply —
 * that guarantee still comes entirely from the RTC pending_acks/pending_up
 * queues above, which this partition neither reads nor replaces. This
 * partition only ever affects what is *drawn*, never what is *sent*.
 * --------------------------------------------------------------------- */
/* G7: bumped 1 -> 2 to add `sndr` as a fourth length-prefixed string,
 * written after `to`, before `body_len`. msghist_record_decode() accepts
 * BOTH versions: 1 reads id/from/to then body_len exactly as before this
 * task and leaves `sndr` empty; 2 also reads the new field. Without that
 * dual read, every message persisted by pre-G7 firmware would be dropped
 * (decode failure) on the first boot after the upgrade — see msg.c. */
#define MSGHIST_REC_VERSION 2
#define MSGHIST_REC_MAX 420 /* header + 4 full-length names + 320-byte body + crc, see msg.c */

/* Pure record codec — no ESP-IDF/NVS dependency, host-tested by
 * firmware/host/test_msg.c the same way msg.c's composer section above is
 * (this whole block sits above msg.c's own `#ifdef ESP_PLATFORM` split).
 * Encoding is variable-length (a one-byte length prefix ahead of each of
 * id/from/to/sndr, a two-byte length ahead of body, not a fixed
 * 17/17/17/17/321-byte layout) so a short "ok" reply costs far fewer NVS
 * bytes than a full 320-byte page — NVS blobs of different sizes coexist
 * fine across successive writes under the same key. Returns the encoded
 * length, or 0 if it cannot possibly fit `out_cap` (never happens for
 * `out_cap >= MSGHIST_REC_MAX`, msg.c's own buffer size). */
size_t msghist_record_encode(const msg_t *m, uint32_t seq, uint8_t *out, size_t out_cap);

/* Decodes a record written by msghist_record_encode(). Returns false (and
 * leaves `out`/`out_seq` untouched) for anything that doesn't check out: an
 * unrecognised version (below 1 or above MSGHIST_REC_VERSION), a truncated
 * buffer, or a CRC mismatch (a torn write from a power loss mid-
 * nvs_commit(), or a key from a future/older firmware) — msg.c's
 * history_restore() treats false as "skip this slot", never fatal, the
 * same corruption-handling rule this codebase uses everywhere else (see
 * e.g. gfx.c's asset-header validation). A version-1 record (pre-G7,
 * no `sndr` field) decodes successfully with `out->sndr == ""`. */
bool msghist_record_decode(const uint8_t *buf, size_t len, msg_t *out, uint32_t *out_seq);

/* True once `ack_state` is the LAST state this direction's ack state
 * machine ever reaches (down: MSG_ACK_READ; up: MSG_ACK_UP_SENT or
 * MSG_ACK_UP_FAILED) — see the write-policy note above for why this is
 * the one state change worth a second NVS write. */
bool msghist_is_terminal_ack(uint8_t dir, uint8_t ack_state);

/* Sorts the index set [0, n) by seqs[i] DESCENDING (newest first) into
 * order_out[0 .. return value - 1] — pure index arithmetic, no msg_t/NVS
 * knowledge, so history_restore()'s "which of the (up to 32) decoded
 * records is newest" step is host-testable on its own. `n` is clamped to
 * `max_out` (defensive; msg.c's only caller already bounds `n` by
 * MSG_THREAD_DEPTH, so this never actually clamps in practice). Returns
 * the number of indices written. */
int msghist_restore_order(const uint32_t *seqs, int n, int *order_out, int max_out);

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

/* F6.4: body no longer stored here — full fidelity lives in NVS namespace
 * `msgq`, key by slot index (msg.c's msgq_write_reply()/msgq_read_reply()).
 * This is metadata only: enough to retry the publish (id, to) and to know
 * whether the entry is stale (created_us). */
typedef struct {
    int64_t created_us; /* esp_timer_get_time() at queue time — see header note, monotonic */
    char id[MSG_ID_MAX];
    char to[MSG_TO_MAX]; /* peer alias; empty = default, §5.6 */
    uint8_t attempts;
    bool in_use;
} msg_pending_up_t;

/* F6.4: body no longer stored here — full fidelity lives in NVS namespace
 * `msgq`, key "unread" (msg.c's msgq_write_unread()/msgq_read_unread()).
 * See this header's second deviation note for why there is no `to` field
 * here (PROTOCOL.md §9.3's table lists one; a down message has none). */
typedef struct {
    int64_t ts;
    char id[MSG_ID_MAX];
    char from[MSG_FROM_MAX];
    bool in_use;
    /* G7: deliberately NO `sndr` field here — pager_rtc_t's 1184-byte
     * budget is nearly full (this struct's own module comment above). The
     * only cost is that warm_recover_unread()/cold_recover_unread()'s
     * single reconstructed row (used only when `msghist` itself is
     * unavailable) loses its group-author line; msghist-restored rows are
     * unaffected since they carry `sndr` in the NVS record, not here. */
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

/* F3.6 (docs/PROTOCOL.md §14, docs/DEVICE_PLAN.md §2.5/§2.7): binds the
 * auth_rtc_t sub-struct modes.c embeds inside its own pager_rtc_t (the same
 * pattern msg_bind_rtc() above uses for msg_rtc_t) so this file's
 * publish/ingest paths can call auth_next_up_n()/auth_accept_down_n()
 * directly, under the already-bound msg_rtc_lock_fn/msg_rtc_save_fn.
 * `on_wrap` is called synchronously, lock NOT held, the moment
 * auth_next_up_n() reports the `up_lo` half of the /up,/status,/loc counter
 * just wrapped — the caller (modes.c) must bump and persist ident's
 * n_epoch via ident_store() before the next signed publish, or the relay's
 * replay window sees `n` go backwards (auth.h's own doc comment). May be
 * NULL if `rtc` is NULL (unsigned build/test double). */
typedef void (*msg_epoch_wrap_fn)(void);
void msg_bind_auth(auth_rtc_t *rtc, msg_epoch_wrap_fn on_wrap);

/* ---------------------------------------------------------------------
 * Public API.
 * --------------------------------------------------------------------- */

/* cold boot -> zero the RAM thread; RTC's own zeroing is modes.c's job via
 * rtc_cold_init(). F6.4: regardless of rtc_was_valid, msg.c also sweeps NVS
 * namespace `msgq` for a pending reply and/or the newest-unread body that
 * outlived the reset (docs/PROTOCOL.md §9.2's durability claim) and
 * reconstructs the minimum RTC/RAM state to keep retrying/showing them —
 * on a cold boot this is the ONLY way that state survives, since RTC itself
 * was just zeroed. On a warm reset (rtc_was_valid==true) RTC's own
 * metadata is trusted for *whether* something exists; NVS is still the
 * only place the body itself is read from (§9.4). Must be called after
 * msg_bind_rtc(). */
void msg_init(bool rtc_was_valid);

typedef enum {
    MSG_INGEST_NEW,
    MSG_INGEST_DUPLICATE,
    MSG_INGEST_MALFORMED,
} msg_ingest_t;

/* F3.6 (docs/PROTOCOL.md §14.3/§14.4, §10 keymap), F6.4 (docs/DEVICE_PLAN.md
 * H14 "CBOR for devices"): the one and only down-ingest path. Every device
 * decodes CBOR regardless of `ident`'s IDENT_FLAG_REQ_SIG — JSON was only
 * ever a stop-gap for req_sig==0 devices until this task, and is gone
 * (cJSON dropped from CMakeLists.txt too). Caller contract: `buf` MUST
 * already have passed auth_verify() when IDENT_FLAG_REQ_SIG is set (trims
 * the ten trailing `sig` bytes without rewriting the map header's declared
 * pair count — see the accounting note inline in msg.c); for a req_sig==0
 * device `buf` is used as received, unverified, and this function does NOT
 * require key 12 (`n`) or check the replay window in that case (§10's
 * keymap: `n` only exists on signed envelopes).
 *
 * F7.1: caller contract also requires `buf` to have already been rejected
 * by book_ingest_cbor()/lock_ingest_cfg_cbor() (both return false for
 * anything that is not their own kind) — modes.c's on_incoming_message()
 * tries those first. This function's own MK_KIND case is defense in depth,
 * not the primary dispatch: an absent `kind` (§3.2's default) or an
 * explicit `"msg"` are the only values accepted here; anything else is
 * MSG_INGEST_MALFORMED.
 *
 * MSG_INGEST_NEW -> caller enters active mode, renders, then calls
 * msg_mark_shown(). MSG_INGEST_DUPLICATE -> re-ack only
 * (msg_mark_shown()/msg_mark_read() as appropriate) — caller MUST NOT
 * re-render, re-alert, or re-enter active mode (§4.1 rule 7). *out may be
 * NULL on this path if the original entry already scrolled out of the RAM
 * thread; the id string handed back via msg_last_ingest_id() is still
 * valid for acking regardless. MSG_INGEST_MALFORMED -> count only, no ack,
 * no render, *out is always NULL. */
msg_ingest_t msg_ingest_down_cbor(const uint8_t *buf, uint16_t len, const msg_t **out);

/* Most recently parsed message id from msg_ingest_down_cbor(), valid for
 * MSG_INGEST_NEW and MSG_INGEST_DUPLICATE (empty string for
 * MSG_INGEST_MALFORMED results where the id field itself did not validate).
 * Exists because a duplicate whose s_thread entry has already scrolled out
 * of the 32-deep RAM ring (which can happen in normal operation: the
 * 16-deep dedup ring is intentionally narrower than the thread now, but a
 * long-offline device can still see this, §4.1 rule 7) still needs to be
 * re-acked by id (§4.1 rule 1), and *out is NULL in that case. */
const char *msg_last_ingest_id(void);

/* down message: state -> shown, queues a pending_ack entry. Returns false
 * only if the pending_acks table was full and the ack could not be queued.
 * v0.2 bug fix #1 (docs/V02_DESIGN.md §2.1): unlike v0.1, the RAM ack_state
 * is NOT advanced when queuing fails -- it stays at whatever it was, so a
 * later call (msg_mark_all_unshown(), a re-render, or scr_chat.c's
 * mark_visible_read()) sees it as still outstanding and retries once a
 * pending_acks slot frees up, instead of the ack being silently lost while
 * the local UI believes it already went out. */
bool msg_mark_shown(const char *id);

/* down message: state -> read, queues a pending_ack entry, clears rtc
 * unread[0] (and its NVS msgq body) if it matches this id. Same
 * queued-before-advanced contract as msg_mark_shown() above. */
bool msg_mark_read(const char *id);

/* F6.5 (docs/DEVICE_PLAN.md §5.8) / v0.2 bug fix #1 (docs/V02_DESIGN.md
 * §2.1): queues a `shown` ack (msg_mark_shown()) for every down message
 * currently at exactly MSG_ACK_UNSHOWN — i.e. every message that arrived
 * while the device was locked and was therefore never displayed (modes.c's
 * handle_ingest_result() skips render_pending_set() for those), AND every
 * message from an incoming burst that overwrote modes.c's single-slot
 * render_pending_t before its own `shown` could be queued (modes.c's
 * service_render_pending() now calls this instead of acking only the last
 * remembered id). Called after a successful unlock (scr_lock.c) and after
 * every successful ui_incoming() render (modes.c). A message the student
 * had already seen (shown or read) is untouched: only ack_state ==
 * MSG_ACK_UNSHOWN entries qualify. Subject to the same MSG_PENDING_ACKS_MAX
 * (8) queue depth as any other ack burst; entries that do not fit are NOT
 * lost (msg_mark_shown()'s new contract above keeps them at UNSHOWN), so
 * the next call — the next unlock, or the next incoming message's render —
 * retries them. */
void msg_mark_all_unshown(void);

/* Student reply. REJECTS (returns false) if the body fails PROTOCOL.md
 * §3.1's rules (empty, a control character, more than 320 UTF-8 bytes, or
 * more than 160 code points) — never truncates (§9.4). Also rejects if
 * pending_up is full (both slots in_use) or the NVS write fails. On
 * success: generates id ("u_" + 8 lowercase hex from esp_random()), writes
 * the full body to NVS namespace `msgq`, inserts into pending_up (metadata
 * only) AND into s_thread (dir=up, ack_state=pending, to=<as given>).
 *
 * F7.3 (docs/DEVICE_PLAN.md §5.5 "Sending from a chat"): `to` is the peer
 * alias to address this reply to, or "" for the default recipient — the
 * wire's own `to` (key 7, publish_reply()) is omitted whenever `to` is ""
 * here, which the caller (scr_chat.c) also uses for "the peer alias equals
 * the default recipient" so the wire stays identical to today's common case.
 * `to` is copied defensively (truncated to MSG_TO_MAX-1) and is NOT itself
 * validated against the address book — scr_chat.c is the only caller and it
 * only ever passes "" or an alias already confirmed against book.h's own
 * accessors (book_contact_at()/book_get_default_alias()), so a second check
 * here would be redundant, not a safety net for anything reachable today. */
bool msg_queue_reply(const char *to, const char *body, uint16_t len);

/* v0.2 §6 (docs/V02_DESIGN.md, sms.c): inserts a DOWN thread entry for an
 * allow-listed inbound SMS. ack_state is set to MSG_ACK_READ IMMEDIATELY
 * (never MSG_ACK_UNSHOWN) so this entry is never shown/read-acked to the
 * relay — PROTOCOL.md §3.6: "those acks mean nothing for an SMS", and
 * msg_mark_all_unshown()'s burst-ack sweep (modes.c) only ever touches
 * MSG_ACK_UNSHOWN entries, so starting here at MSG_ACK_READ is what keeps
 * this id out of that sweep for its entire lifetime. Generates its own id
 * ("x_" + 8 lowercase hex from esp_random() — a namespace no relay-issued
 * id, and no other id this codebase generates ("u_" replies, "l_" /loc,
 * "s_" sms_log audit), ever produces, so it can never collide with or be
 * mistaken for one). `from` is the SMS contact's display name (shown as the
 * `from` column, same as any other down message). Rejects (returns false,
 * nothing inserted) only if `body` fails the same body_rules_ok() every
 * down message is held to — sms.c's own decoder is expected to have already
 * produced valid text, so a false return here means a decoder bug, not
 * something the caller should react to specially. On success, `out_id` is
 * filled with the generated id (for the caller's own alert/audit
 * bookkeeping — modes_alert_incoming(), modes.h). */
bool msg_insert_sms_in(const char *from, const char *body, uint16_t body_len, char *out_id,
                       size_t out_id_cap);

/* v0.2 §6: inserts a PENDING ("...", MSG_ACK_UP_PENDING) UP thread entry for
 * a direct SMS send — deliberately OUTSIDE msg.c's own pending_up/NVS
 * `msgq` queue: this entry is NEVER retried by msg_pump() and NEVER
 * published over MQTT (PROTOCOL.md §3.6: the whole point of this path is
 * that it works without the relay). `to` is the SMS contact's display name
 * (not shown anywhere today — chat_render()'s `who` column is "you" for
 * every up entry regardless of `to`, same pre-existing limitation an
 * ordinary `@alias` reply already has — kept for a future per-peer view).
 * Same body validation and id-generation namespace as msg_insert_sms_in()
 * above (a fresh, independent "x_" id — the two functions never share an
 * id). On success, `out_id` is filled with the generated id, which the
 * caller (sms.c's sms_service()) later hands to msg_finish_sms_out() once
 * the actual smsSend() attempt completes. */
bool msg_insert_sms_out_pending(const char *to, const char *body, uint16_t body_len, char *out_id,
                                size_t out_id_cap);

/* v0.2 §6: resolves a thread entry msg_insert_sms_out_pending() created to
 * its final MSG_ACK_UP_SENT/MSG_ACK_UP_FAILED state (`ok`), by id — the
 * same two terminal states (and the same "sent"/"FAILED" rendering,
 * chat_render()) an ordinary `/up` reply's msg_pump() already produces, so
 * no UI change was needed for this to show correctly. No-op (returns false)
 * if `id` has already scrolled out of the 32-deep RAM thread by the time
 * the send attempt finished — cosmetic only: the sms_log audit entry
 * (sms.c's own audit ring) is the durable record of the outcome, not this
 * UI row. */
bool msg_finish_sms_out(const char *id, bool ok);

/* Called once per wake cycle from modes_run() while the MQTT session is
 * connected. AT MOST ONE publish per call. Priority: pending acks
 * oldest-first (FIFO by array slot order — see msg.c), then pending
 * replies. Acks: 3 attempts then drop. Replies: 3 attempts OR
 * created_us > 2h old -> mark MSG_F_SEND_FAILED on the matching s_thread
 * entry, free the pending_up slot AND erase its NVS `msgq` body (§4.2). */
void msg_pump(void);

size_t msg_thread_count(void);

/* README R6 fix: both accessors take the lock and copy the matching entry
 * into a dedicated static snapshot before returning a pointer to it, so the
 * pointer handed back can never be torn or moved out from under the caller
 * by a concurrent msg_ingest_down_cbor()/msg_queue_reply() (different task)
 * — it is a *copy*, frozen at the moment of the call, not a view into the
 * live, moving s_thread ring. Each function owns its own snapshot slot, so
 * calling one does not invalidate a pointer already returned by the other
 * within the same caller (e.g. scr_chat.c's chat_render() reads
 * msg_newest_unread() once, then msg_thread_at() per row). A second call to
 * the *same* function does overwrite its own snapshot, so callers must
 * finish with one result (copy the fields they need) before requesting the
 * next, exactly as today's callers already do. */
const msg_t *msg_thread_at(size_t index); /* 0 = newest; NULL if index >= msg_thread_count() */
const msg_t *msg_newest_unread(void);     /* NULL if none */

/* Chat/thread queries (docs/DEVICE_PLAN.md §5.6): invokes `cb` once per
 * s_thread entry belonging to peer `alias` — a down message belongs to the
 * peer that sent it (`from == alias`); an up message belongs to the peer it
 * was addressed to (`to == alias`; since no caller sets `to` yet, F6.4's
 * only caller of this would be one that already knows aliases equal "").
 * `from_newest` selects direction (newest-first / oldest-first); `cb`
 * receives a stack COPY (same discipline as msg_thread_at(), README R6),
 * taken under the lock, which is held for the whole iteration — `cb` MUST
 * NOT block, allocate, or call back into msg.c. The peers list itself is
 * not tracked separately; a caller derives it on demand from
 * msg_thread_at()'s from/to fields (docs/DEVICE_PLAN.md §5.6). */
typedef void (*msg_iter_peer_cb)(const msg_t *m, void *ctx);
void msg_iter_peer(const char *alias, bool from_newest, msg_iter_peer_cb cb, void *ctx);

/* true once, after a reset/cold-boot recovery, until this is called for the
 * first time afterward — self-clearing one-shot (chosen semantics: "true
 * once" per the header comment in the phase brief; no separate ack/clear
 * call is exposed since a getter that also clears is simpler for a single
 * UI caller). Always false after a boot with nothing to recover. */
bool msg_history_lost(void);

/* Owner task 2026-09-20 (factory reset must erase the persisted history
 * too — it is the child's private messages): erases every key in the
 * `msghist` NVS partition/namespace, and, for the same reason, the `msgq`
 * namespace's pending-reply/unread bodies (msgq_erase_reply()/
 * msgq_erase_unread(), already used elsewhere in this file for their own
 * per-message lifecycle). No-op if `msghist` was never available (nothing
 * to erase). Does not touch s_thread/RTC — the caller (scr_device.c's
 * factory-reset confirm) always follows this with ident_erase() +
 * esp_restart(), which is what actually clears those. Safe to call more
 * than once; NVS erase of an already-empty namespace is a no-op. */
void msg_history_erase(void);

typedef struct {
    uint32_t dedup_hits;
    uint32_t malformed_drops;
    uint32_t reply_failed;
} msg_stats_t;
void msg_get_stats(msg_stats_t *out);

/* F3.6: increments the malformed_drops counter (§3.4 diagnostics) for a
 * caller that rejects an envelope *before* handing it to
 * msg_ingest_down_cbor() — currently only a failed auth_verify() on the RX
 * path (modes.c). Exposed so that rejection is still counted even though
 * this file never got far enough to parse the envelope. */
void msg_count_malformed(void);

/* ---------------------------------------------------------------------
 * Composer buffer (owned here since msg_queue_reply() is the consumer of
 * its contents; ui.c drives it via these accessors instead of touching the
 * buffer directly, keeping msg.c the single owner of the 320-byte/
 * 160-codepoint limit, docs/DEVICE_PLAN.md §5.2/§9.4).
 *
 * No ESP-IDF dependency (host-tested by firmware/host/test_msg.c, same
 * `#ifdef ESP_PLATFORM` split as input.c/auth.c): every text buffer on the
 * device is UTF-8 "from day one" per §5.2, but the CardKB and the only IME
 * built so far (ime.h's identity IME) still feed one byte at a time via
 * ime_result_t.commit_utf8's loop in scr_chat.c, so msg_composer_push_char()
 * keeps that exact one-byte-at-a-time signature and instead buffers an
 * in-progress multi-byte UTF-8 sequence internally, only checking the caps
 * and committing once a whole code point has arrived — a code point is
 * accepted or refused atomically, never split across the 320-byte/
 * 160-codepoint boundary (§9.4: "refusing further input rather than
 * truncating").
 * --------------------------------------------------------------------- */
void msg_composer_reset(void);
/* Returns false (and leaves the buffer + code point count unchanged) if
 * completing the in-progress UTF-8 sequence with `c` would exceed either
 * MSG_COMPOSER_MAX-1 bytes or MSG_COMPOSER_MAX_CODEPOINTS code points. A
 * byte that only continues an incomplete sequence (the cap check hasn't
 * happened yet) returns true without changing either count. */
bool msg_composer_push_char(char c);
/* Deletes the last whole code point (not just the last byte), and cancels
 * any in-progress incomplete sequence. */
bool msg_composer_backspace(void);
const char *msg_composer_text(void);
uint16_t msg_composer_len(void);             /* bytes committed so far */
uint16_t msg_composer_codepoint_count(void); /* code points committed so far */

#ifdef __cplusplus
}
#endif

#endif /* MSG_H */

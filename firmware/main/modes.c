// modes.c — mode state machine, RTC memory contract, wake-and-drain loop,
// button short/long press state machine.
//
// Authority: docs/PROTOCOL.md §4.1 (ack rules), §5.4 (status cadence), §8
// (wake sources), §9 (RTC memory, rewritten Phase 5), §11 (mode funnel).
//
// modes.c is the only place that touches the RTC struct and the only place
// that changes `mode` (funnelled through set_mode(), per §11). It owns the
// button state machine (Phase 5 Part C) and wires msg.c/ui.c together.
//
// All power-effect comments are PENDING_HW.

#include "modes.h"
#include "msg.h"
#include "net.h"
#include "pins.h"
#include "ui.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "modes";

// ---------------------------------------------------------------------------
// Compile-time constants. PAGER_WAKE_INTERVAL_* / PAGER_ACTIVE_IDLE_TIMEOUT_S
// are deliberately named to match PROTOCOL.md §8.2 so they can be retuned
// against a real current trace without touching logic.
// ---------------------------------------------------------------------------

#define PAGER_WAKE_INTERVAL_SLEEP_MS 5000u  // T=5s sleep mode, PROTOCOL.md §8.2
#define PAGER_WAKE_INTERVAL_ACTIVE_MS 2000u // T=2s active mode
#define PAGER_POST_WAKE_YIELD_MS 50u        // >=30ms floor (L4); 50ms per §8.2's own margin
#define PAGER_ACTIVE_IDLE_TIMEOUT_S (10 * 60) // 10 min, HANDOFF.md §2
#define PAGER_STATUS_HEARTBEAT_S 3600u         // §5.4(d)
#define PAGER_CHECKCOMM_EVERY_N_WAKES 60u      // F4: ~5 min at T=5s
#define PAGER_MODEM_RESET_MIN_INTERVAL_US ((int64_t) 10 * 60 * 1000000) // F4 rate limit
#define PAGER_FW_VERSION "0.1.0"

// Part C: button short/long press state machine.
#define PAGER_BTN_LONG_PRESS_MS 600u
#define PAGER_BTN_DEBOUNCE_MS 30u
#define PAGER_BTN_POLL_MS 20u // polling granularity while the FSM is not IDLE

// F1/F3 backoff schedule: 5s, 15s, 60s, 300s, then steady at 300s.
static const uint32_t k_backoff_s[] = { 5, 15, 60, 300 };
#define PAGER_BACKOFF_STEPS (sizeof(k_backoff_s) / sizeof(k_backoff_s[0]))

// ---------------------------------------------------------------------------
// RTC memory contract (PROTOCOL.md §9, rewritten Phase 5). The dedup/ack/
// reply/unread state now lives in msg_rtc_t (msg.h), embedded here as a
// nested `msg` field; modes.c remains the sole owner of the enclosing
// struct, its magic/crc32 pair and rtc_save() (§9.3), and hands msg.c a
// typed pointer plus lock/unlock/save callbacks via msg_bind_rtc().
//
// Phase 4 shipped a 48-byte-snippet stopgap here (PAGER_RTC_SNIPPET_BYTES /
// PAGER_MSG_RING_MAX / PAGER_PENDING_UP_MAX) because the measured 7003-byte
// walter-modem RTC footprint (see sdkconfig.defaults) left only ~1184 of
// the 8192-byte RTC_SLOW region for this struct — far short of PROTOCOL.md
// §9's original full-body sizing. Phase 5's storage-contract rewrite (§9.3)
// resolves that by keeping full bodies out of RTC entirely (they live in
// msg.c's RAM-resident s_thread, §9.5) and only mirroring the single
// newest-unread message plus small id-only queues in RTC. That whole
// stopgap approach is superseded; nothing from it survives below.
// ---------------------------------------------------------------------------

#define PAGER_ID_MAX_LEN 17 // 16 chars + NUL, PROTOCOL.md §1/§3.1

// magic encodes both validity and a layout version tag (per CLAUDE.md's RTC
// convention: "a version tag and a CRC"). Bumped from Phase 4's layout
// (0x50475231): the msg_rtc_t sub-struct below is binary-incompatible with
// Phase 4's pending_acks/pending_up/msg_ring arrays, and a stale-but-CRC-
// valid struct read across that change would otherwise decode as garbage.
#define PAGER_RTC_MAGIC 0x50475232u // "PGR" + layout version 2

typedef enum {
    PAGER_MODE_SLEEP = 0,
    PAGER_MODE_ACTIVE = 1,
} pager_mode_t;

typedef enum {
    MODE_REASON_BOOT = 0,
    MODE_REASON_BUTTON,
    MODE_REASON_INCOMING_MSG,
    MODE_REASON_IDLE_TIMEOUT,
} pager_mode_reason_t;

typedef struct {
    uint32_t magic;
    uint32_t crc32;

    uint32_t boot_count;
    char session_id[12]; // "s_" + 8 hex + NUL, PROTOCOL.md §1

    uint8_t mode; // pager_mode_t
    // Part A bug #1 fix: this is a MONOTONIC deadline (esp_timer_get_time()
    // microseconds), not wall-clock epoch seconds. The Phase 4 version
    // compared against approx_epoch(), which reads 0 until the network
    // clock arrives (§3.5) — combined with the "!=0" guard on the exit
    // check, that meant a device with no clock yet never left active mode
    // (stuck at 7-17mA instead of 1.8-2.1mA), and then the window could
    // slam shut instantly once epoch time first became available and
    // happened to already exceed the stale deadline computed against 0.
    // A monotonic clock has no "not available yet" state, so this class of
    // bug cannot recur here.
    int64_t active_until_us;

    uint32_t status_pub_count;
    int64_t last_status_epoch;

    msg_rtc_t msg; // PROTOCOL.md §9.3 — owned in layout by modes.c, in
                    // behaviour by msg.c via msg_bind_rtc().

    uint32_t mqtt_memfull_count; // §8.4 M6, cumulative
    uint32_t oversize_drop_count; // F6, cumulative

    uint32_t modem_resets;         // F4, cumulative
    int64_t last_modem_reset_us;   // esp_timer_get_time() at last reset, for the 10min rate limit

    uint32_t attach_fail_cycles; // F1
    uint32_t wake_cycle_count;   // drives the "every 60 wakes" F4 check
} pager_rtc_t;

_Static_assert(sizeof(pager_rtc_t) <= 1184,
               "pager_rtc_t exceeds the measured 1184-byte RTC_SLOW budget "
               "left after walter-modem's own ~7003 bytes (PROTOCOL.md §9.1)");

RTC_DATA_ATTR static pager_rtc_t g_rtc;

// Guards g_rtc against the race between the wake-and-drain loop below and
// the MQTT MESSAGE callback (WalterModem's _eventProcessingTask), and is
// reused (msg.h's header comment explains why) to guard msg.c's RAM-resident
// thread/composer state too. Statically allocated: no heap use after init.
static StaticSemaphore_t s_rtc_mutex_buf;
static SemaphoreHandle_t s_rtc_mutex;

static void rtc_lock(void) { xSemaphoreTake(s_rtc_mutex, portMAX_DELAY); }
static void rtc_unlock(void) { xSemaphoreGive(s_rtc_mutex); }

static bool s_was_mqtt_connected = false;

// ---------------------------------------------------------------------------
// RTC helpers
// ---------------------------------------------------------------------------

static uint32_t rtc_compute_crc(void)
{
    // CRC over everything except the magic+crc32 header itself.
    const uint8_t *start = (const uint8_t *) &g_rtc.boot_count;
    size_t len = sizeof(g_rtc) - offsetof(pager_rtc_t, boot_count);
    return esp_rom_crc32_le(0, start, len);
}

static bool rtc_is_valid(void)
{
    return g_rtc.magic == PAGER_RTC_MAGIC && g_rtc.crc32 == rtc_compute_crc();
}

static void rtc_save(void)
{
    g_rtc.magic = PAGER_RTC_MAGIC;
    g_rtc.crc32 = rtc_compute_crc();
}

static void gen_session_id(char *out, size_t out_size)
{
    // "s_" + 8 lowercase hex, PROTOCOL.md §1.
    snprintf(out, out_size, "s_%08x", (unsigned) esp_random());
}

static void rtc_cold_init(void)
{
    memset(&g_rtc, 0, sizeof(g_rtc));
    g_rtc.boot_count = 1;
    gen_session_id(g_rtc.session_id, sizeof(g_rtc.session_id));
    g_rtc.mode = (uint8_t) PAGER_MODE_SLEEP;
    rtc_save();
}

// Best-effort wall clock; returns 0 if the network clock was never obtained
// (PROTOCOL.md §3.5 - the caller must then treat ts as 0, not retry SNTP).
static int64_t approx_epoch(void)
{
    int64_t e;
    if (net_get_clock(&e)) {
        return e;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Status (/status) publishing - PROTOCOL.md §5.
// ---------------------------------------------------------------------------

static bool build_status_json(char *out, size_t out_size, const char *state)
{
    int64_t ts = approx_epoch();
    const char *mode_str = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE) ? "active" : "sleep";
    // batt_mv: no ADC pin is defined in pins.h for battery sense (out of
    // scope for this phase - see the final report's NEEDS HUMAN DECISION
    // note). Placeholder mid-range LiFePO4 value so the payload is at least
    // schema-valid; PENDING_HW for a real reading.
    const int batt_mv_placeholder = 3300;
    int n = snprintf(out, out_size,
                      "{\"v\":1,\"state\":\"%s\",\"mode\":\"%s\",\"batt_mv\":%d,"
                      "\"session\":\"%s\",\"ts\":%lld,\"fw\":\"%s\"}",
                      state, mode_str, batt_mv_placeholder, g_rtc.session_id, (long long) ts,
                      PAGER_FW_VERSION);
    return n > 0 && (size_t) n < out_size;
}

static void publish_status_online(void)
{
    char json[192];
    if (!build_status_json(json, sizeof(json), "online")) {
        ESP_LOGI(TAG, "status JSON build failed (buffer too small)");
        return;
    }
    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/status", net_get_device_id());

    if (net_publish(topic, json, (uint16_t) strlen(json), 1)) {
        rtc_lock();
        g_rtc.status_pub_count++;
        g_rtc.last_status_epoch = approx_epoch();
        rtc_unlock();
        ESP_LOGI(TAG, "published /status online (mode=%s)",
                 g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE ? "active" : "sleep");
    } else {
        ESP_LOGI(TAG, "publish /status online failed");
    }
}

static void maybe_publish_heartbeat(void)
{
    // §5.4(d): heartbeat rides an ordinary poll wake, at most once per
    // 3600s - not an independent timer (that would cost an extra radio
    // wake for nothing, per §5.4's own rationale).
    int64_t now = approx_epoch();
    if (now == 0) {
        return; // no clock yet; don't spam ts:0 heartbeats
    }
    if (now - g_rtc.last_status_epoch < (int64_t) PAGER_STATUS_HEARTBEAT_S) {
        return;
    }
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (!st.mqtt_connected) {
        return;
    }
    publish_status_online();
}

// ---------------------------------------------------------------------------
// set_mode() — the single transition funnel required by PROTOCOL.md §11.
// ---------------------------------------------------------------------------

static const char *mode_name(pager_mode_t m)
{
    return (m == PAGER_MODE_ACTIVE) ? "active" : "sleep";
}

static void set_mode(pager_mode_t new_mode, pager_mode_reason_t reason)
{
    rtc_lock();
    bool changed = ((pager_mode_t) g_rtc.mode != new_mode);
    if (changed) {
        ESP_LOGI(TAG, "mode: %s -> %s (reason=%d)", mode_name((pager_mode_t) g_rtc.mode),
                 mode_name(new_mode), (int) reason);
        g_rtc.mode = (uint8_t) new_mode;
    }
    if (new_mode == PAGER_MODE_ACTIVE) {
        // Part A bug #2 fix: refresh the deadline on every call while
        // active, not only on the sleep->active edge (Phase 4's early
        // return on "mode unchanged" meant 10 minutes was measured from
        // mode *entry*, not from the last activity - HANDOFF.md §2 requires
        // the latter). modes_note_activity() below is the lightweight path
        // for this; set_mode() also refreshes it here so a fresh incoming
        // message or button press extends the window even though it also
        // happens to already be active.
        g_rtc.active_until_us = esp_timer_get_time() + (int64_t) PAGER_ACTIVE_IDLE_TIMEOUT_S * 1000000;
    }
    rtc_save();
    rtc_unlock();

    if (!changed) {
        return;
    }

    // §5.4(b): publish on every mode change, but only if the session is
    // actually usable - otherwise this just becomes a queued publish that
    // races the next connect's own online announcement.
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (st.mqtt_connected) {
        publish_status_online();
    }
    ui_render_thread(); // status line shows mode; only worth a repaint on a real edge
}

void modes_note_activity(void)
{
    rtc_lock();
    bool active = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE);
    if (active) {
        g_rtc.active_until_us = esp_timer_get_time() + (int64_t) PAGER_ACTIVE_IDLE_TIMEOUT_S * 1000000;
        rtc_save();
    }
    rtc_unlock();
}

bool modes_is_active(void)
{
    rtc_lock();
    bool active = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE);
    rtc_unlock();
    return active;
}

// ---------------------------------------------------------------------------
// Incoming message hook — wired to msg.c's ingest/dedup/ack state machine.
// ---------------------------------------------------------------------------

static void on_incoming_message(const char *topic, const char *body, uint16_t len)
{
    (void) topic;
    const msg_t *out = NULL;
    msg_ingest_t r = msg_ingest_down(body, len, &out);

    switch (r) {
    case MSG_INGEST_NEW: {
        // Phase 6 fix: copy the id BEFORE rendering. `out` points into
        // msg.c's s_thread ring, which thread_insert_locked() memmoves on
        // every insert; a reply submitted from modes_run()'s task (Enter /
        // long-press -> msg_queue_reply()) while this render is in flight
        // would shift s_thread[0] and make out->id the reply's "u_..." id,
        // acking a message the relay has never heard of (§4.1 rule 3) and
        // leaving the real down message un-acked forever.
        char id[MSG_ID_MAX] = "";
        if (out) {
            strncpy(id, out->id, sizeof(id) - 1);
        }
        set_mode(PAGER_MODE_ACTIVE, MODE_REASON_INCOMING_MSG);
        // §4: the shown ack is queued only after the render call returns,
        // which (ui_render_message_pane -> ... -> disp_wait_busy()) blocks
        // until BUSY deasserts - satisfies "after the e-paper refresh
        // completes, never before". The actual /up publish happens later,
        // asynchronously, from msg_pump().
        ui_render_message_pane();
        if (id[0] != '\0') {
            msg_mark_shown(id);
        }
        break;
    }

    case MSG_INGEST_DUPLICATE: {
        // §4.1 rule 7: re-ack only, MUST NOT re-render/re-alert/re-enter
        // active mode. *out is NULL if the entry already scrolled out of
        // the 10-deep RAM thread (routine: the 16-deep dedup ring is
        // intentionally wider) - fall back to the id msg.c just parsed.
        const char *id = out ? out->id : msg_last_ingest_id();
        if (id[0] != '\0') {
            if (out && out->ack_state == MSG_ACK_READ) {
                msg_mark_read(id);
            } else {
                msg_mark_shown(id);
            }
        }
        break;
    }

    case MSG_INGEST_MALFORMED:
    default:
        // §3.4: log, count (msg.c already counted it), do not ack, do not
        // render, do not reboot.
        ESP_LOGD(TAG, "malformed down message dropped (%u bytes)", (unsigned) len);
        break;
    }
}

// ---------------------------------------------------------------------------
// F1/F3: MQTT session backoff
// ---------------------------------------------------------------------------

static void schedule_backoff(uint32_t *backoff_index, int64_t *next_retry_us)
{
    uint32_t idx = *backoff_index;
    uint32_t delay_s = k_backoff_s[idx < PAGER_BACKOFF_STEPS ? idx : PAGER_BACKOFF_STEPS - 1];
    *next_retry_us = esp_timer_get_time() + (int64_t) delay_s * 1000000;
    if (idx < PAGER_BACKOFF_STEPS) {
        (*backoff_index)++;
    }
}

static void pin_to_steady_backoff(uint32_t *backoff_index, int64_t *next_retry_us)
{
    *backoff_index = PAGER_BACKOFF_STEPS; // steady 300s from here on
    *next_retry_us = esp_timer_get_time() + (int64_t) k_backoff_s[PAGER_BACKOFF_STEPS - 1] * 1000000;
}

static void handle_mqtt_loss(const net_mqtt_status_t *st, uint32_t *backoff_index,
                              int64_t *next_retry_us)
{
    switch (st->last_class) {
    case NET_MQTT_RC_PERMANENT:
        ESP_LOGI(TAG, "MQTT permanently refused (rc=%d): CONN_REFUSED/AUTH/ACL_DENIED - "
                      "steady 300s backoff, check broker credentials/ACLs",
                 st->last_rc);
        pin_to_steady_backoff(backoff_index, next_retry_us);
        break;
    case NET_MQTT_RC_TLS_FAIL:
        ESP_LOGI(TAG, "MQTT TLS handshake failed (rc=%d): provisioning bug, not transient - "
                      "steady 300s backoff, check cert slot / TLS profile id (PROTOCOL.md §6.1)",
                 st->last_rc);
        pin_to_steady_backoff(backoff_index, next_retry_us);
        break;
    case NET_MQTT_RC_TRANSIENT:
    default:
        schedule_backoff(backoff_index, next_retry_us);
        ESP_LOGI(TAG, "MQTT session lost (rc=%d), transient - retrying with backoff", st->last_rc);
        break;
    }
    net_ack_disconnect_edge();
}

// ---------------------------------------------------------------------------
// F4: modem health check
// ---------------------------------------------------------------------------

static void run_modem_health_check(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (net_check()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    int64_t now_us = esp_timer_get_time();
    if (g_rtc.last_modem_reset_us != 0 &&
        (now_us - g_rtc.last_modem_reset_us) < PAGER_MODEM_RESET_MIN_INTERVAL_US) {
        ESP_LOGI(TAG, "modem unresponsive but reset is rate-limited (last reset %lld s ago) - "
                      "not resetting again yet",
                 (long long) ((now_us - g_rtc.last_modem_reset_us) / 1000000));
        return;
    }

    ESP_LOGI(TAG, "modem unresponsive after 3 retries over 3s; resetting (F4)");
    rtc_lock();
    g_rtc.modem_resets++;
    g_rtc.last_modem_reset_us = now_us;
    rtc_save();
    rtc_unlock();

    if (!net_recover_modem()) {
        ESP_LOGI(TAG, "modem recovery failed");
        return;
    }
    net_session_up();
}

// ---------------------------------------------------------------------------
// Part C: button short/long press state machine. Replaces Phase 4's
// handle_button_wake(), which could not distinguish short from long press
// and, worse, used a LEVEL wake (esp_sleep_enable_ext0_wakeup(..., 0)):
// esp_light_sleep_start() returns immediately for as long as the button is
// held, so a held button busy-looped the wake-and-drain cycle at ~40mA -
// roughly a day and a half to drain the cell. modes_run() now skips
// net_sleep() entirely whenever this FSM is not IDLE (same pattern as the
// composer-open carve-out), which both fixes that battery bug and gives the
// FSM the frequent polling it needs to measure press duration.
// ---------------------------------------------------------------------------

typedef enum {
    BTN_IDLE = 0,
    BTN_DOWN,
    BTN_HELD,
} btn_state_t;

static btn_state_t s_btn_state = BTN_IDLE;
static int64_t s_btn_t0_us = 0;
static int64_t s_btn_debounce_start_us = 0;

static void button_gpio_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PAGER_PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // active low
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

// Dispatch table (Part C). Every resolved press (short or long) calls
// modes_note_activity() from button_fsm_step() before dispatching here.

static void composer_try_submit(void)
{
    if (msg_queue_reply(msg_composer_text(), msg_composer_len())) {
        ui_composer_close(true);
    } else {
        ui_show_toast("reply too long or send queue full");
    }
}

static void button_action_short(void)
{
    if (ui_composer_is_open()) {
        ui_composer_close(false); // cancel
        return;
    }
    const msg_t *u = msg_newest_unread();
    if (u) {
        char id[MSG_ID_MAX];
        strncpy(id, u->id, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
        msg_mark_read(id);
        ui_render_message_pane();
    } else {
        ui_composer_open(NULL);
    }
}

static void button_action_long(void)
{
    if (ui_composer_is_open()) {
        composer_try_submit();
        return;
    }
    // Unread-or-not, a long press with the composer closed has nothing to
    // send yet (composing hasn't started) - Part C's dispatch table.
    ui_show_toast("nothing to send");
}

static void button_fsm_step(int level, int64_t now_us)
{
    switch (s_btn_state) {
    case BTN_IDLE:
        if (level == 0) {
            if (s_btn_debounce_start_us == 0) {
                s_btn_debounce_start_us = now_us;
            } else if ((now_us - s_btn_debounce_start_us) >=
                       (int64_t) PAGER_BTN_DEBOUNCE_MS * 1000) {
                s_btn_state = BTN_DOWN;
                s_btn_t0_us = now_us;
                s_btn_debounce_start_us = 0;
                // HANDOFF.md §2: button press enters active mode.
                set_mode(PAGER_MODE_ACTIVE, MODE_REASON_BUTTON);
            }
        } else {
            s_btn_debounce_start_us = 0;
        }
        break;

    case BTN_DOWN:
        if (level != 0) {
            // Released before the long-press threshold: short press.
            modes_note_activity();
            button_action_short();
            s_btn_state = BTN_IDLE;
        } else if ((now_us - s_btn_t0_us) >= (int64_t) PAGER_BTN_LONG_PRESS_MS * 1000) {
            // Fire at 600ms elapsed - do not wait for release.
            modes_note_activity();
            button_action_long();
            s_btn_state = BTN_HELD;
        }
        break;

    case BTN_HELD:
        if (level != 0) {
            s_btn_state = BTN_IDLE;
        }
        break;
    }
}

static bool button_fsm_active(void) { return s_btn_state != BTN_IDLE; }

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void modes_boot(void)
{
    s_rtc_mutex = xSemaphoreCreateMutexStatic(&s_rtc_mutex_buf);

    bool was_valid = rtc_is_valid();
    if (was_valid) {
        // Survived a watchdog/software reset (not EN/brownout/power-cycle,
        // which lose RTC memory per PROTOCOL.md §9) - keep session_id and
        // any pending queues rather than silently dropping a student's
        // reply or ack.
        g_rtc.boot_count++;
        ESP_LOGI(TAG, "recovered RTC state across reset (boot_count=%u, session=%s)",
                 (unsigned) g_rtc.boot_count, g_rtc.session_id);
    } else {
        rtc_cold_init();
        ESP_LOGI(TAG, "cold boot, new session=%s", g_rtc.session_id);
    }
    rtc_save();

    ESP_LOGI(TAG, "pager_rtc_t size = %u bytes (of the 1184-byte budget left "
                  "after walter-modem's own ~7003 bytes, PROTOCOL.md §9.1)",
             (unsigned) sizeof(g_rtc));

    msg_bind_rtc(&g_rtc.msg, rtc_lock, rtc_unlock, rtc_save);
    msg_init(was_valid);

    button_gpio_init();

    if (!ui_init()) {
        ESP_LOGI(TAG, "display init failed; continuing headless (network/replies/acks unaffected)");
    } else {
        ui_render_thread();
    }

    net_set_msg_cb(on_incoming_message);

    if (!net_init()) {
        ESP_LOGI(TAG, "net_init() failed at boot; will retry from the wake loop (F1)");
        rtc_lock();
        g_rtc.attach_fail_cycles++;
        rtc_unlock();
    } else {
        rtc_lock();
        g_rtc.attach_fail_cycles = 0;
        rtc_unlock();
        if (!net_session_up()) {
            ESP_LOGI(TAG, "net_session_up() failed at boot; will retry per F3 backoff");
        }
    }

    rtc_lock();
    g_rtc.mode = (uint8_t) PAGER_MODE_SLEEP; // HANDOFF.md §2: boot in sleep mode
    rtc_save();
    rtc_unlock();

    ESP_LOGI(TAG, "boot complete, entering sleep mode");
}

void modes_run(void)
{
    uint32_t backoff_index = 0;
    int64_t next_session_retry_us = 0; // 0 = retry as soon as we notice we're down

    for (;;) {
        uint32_t interval_ms = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE)
                                    ? PAGER_WAKE_INTERVAL_ACTIVE_MS
                                    : PAGER_WAKE_INTERVAL_SLEEP_MS;

        // F5: a non-zero MEMORY_FULL count is direct evidence the drain
        // cycle is falling behind. Shorten T defensively rather than let
        // messages keep piling up in the modem's own buffer.
        uint32_t memfull_delta = net_take_memfull_delta();
        if (memfull_delta > 0) {
            rtc_lock();
            g_rtc.mqtt_memfull_count += memfull_delta;
            uint32_t cumulative = g_rtc.mqtt_memfull_count;
            rtc_unlock();
            ESP_LOGI(TAG, "MQTT MEMORY_FULL x%u this cycle (cumulative %u) - shortening wake interval",
                     (unsigned) memfull_delta, (unsigned) cumulative);
            interval_ms = interval_ms / 2;
            if (interval_ms < 1000) {
                interval_ms = 1000;
            }
        }

        uint32_t oversize_delta = net_take_oversize_delta();
        if (oversize_delta > 0) {
            rtc_lock();
            g_rtc.oversize_drop_count += oversize_delta;
            rtc_unlock();
            ESP_LOGI(TAG, "%u oversize MQTT payload(s) dropped this cycle (F6)",
                     (unsigned) oversize_delta);
        }

        // Part A bug #3 fix / Part C: skip net_sleep() entirely whenever the
        // composer is open (existing carve-out) OR the button FSM is not
        // IDLE (new carve-out - a held button must not re-enter a level-
        // triggered light sleep it would just immediately exit again).
        // Phase 6: net_modem_busy() joins the carve-out. The MQTT event
        // handler runs at priority 4 against this task's priority 1, so it
        // hands the CPU back here every time it blocks; without this term
        // net_sleep() deasserts RTS in the middle of the modem's response
        // to mqttReceive() (see net.h). Costs ~1.5s of 40mA busy-polling
        // per incoming message (~0.02 mAh, ~0.4 mAh/day at 20 msgs/day,
        // estimate) and buys back a per-message message-loss window.
        bool skip_sleep = ui_composer_is_open() || button_fsm_active() || net_modem_busy();
        if (!skip_sleep) {
            net_sleep(interval_ms);
            // L4/F7: the event task ticks at 10ms + settles for 10ms; give
            // it >=30ms of awake time before looking at anything it may
            // have produced.
            vTaskDelay(pdMS_TO_TICKS(PAGER_POST_WAKE_YIELD_MS));
            assert(PAGER_POST_WAKE_YIELD_MS >= 30); // F7, debug builds only
        } else if (ui_composer_is_open()) {
            vTaskDelay(pdMS_TO_TICKS(100)); // HANDOFF.md §2: CardKB polled at 100ms
        } else {
            vTaskDelay(pdMS_TO_TICKS(PAGER_BTN_POLL_MS)); // button FSM debounce/timing granularity
        }

        button_fsm_step(gpio_get_level((gpio_num_t) PAGER_PIN_BUTTON), esp_timer_get_time());

        if (ui_composer_is_open()) {
            ui_key_t key = ui_poll_keys();
            if (key.type == UI_KEYTYPE_ENTER) {
                // §9.4/Part D: Enter alone submits, without also requiring
                // the button long-press - chosen for CardKB-only
                // convenience. The button long-press remains a working
                // alternative submit path for one-handed operation.
                composer_try_submit();
            }
        }

        rtc_lock();
        g_rtc.wake_cycle_count++;
        uint32_t wake_cycle_count = g_rtc.wake_cycle_count;
        rtc_unlock();

        net_mqtt_status_t st;
        net_get_mqtt_status(&st);

        if (st.mqtt_connected && !s_was_mqtt_connected) {
            // Edge: session just became usable. §5.4a - drives the relay's
            // re-publish of unacked messages (§5.3).
            publish_status_online();
        }
        s_was_mqtt_connected = st.mqtt_connected;

        if (st.disconnect_edge) {
            handle_mqtt_loss(&st, &backoff_index, &next_session_retry_us);
        } else if (!st.mqtt_connected) {
            if (esp_timer_get_time() >= next_session_retry_us) {
                ESP_LOGI(TAG, "retrying MQTT session (backoff idx=%u)", (unsigned) backoff_index);
                if (!net_session_up()) {
                    schedule_backoff(&backoff_index, &next_session_retry_us);
                }
            }
        } else {
            // Healthy connection: reset backoff so a future loss starts
            // from 5s again rather than wherever it left off.
            backoff_index = 0;
        }

        // Part B: at most one publish per wake cycle, only while connected,
        // and only on a real wake-and-drain cycle (not the busy-poll
        // cadence used while the composer/button FSM keep us from
        // sleeping) - PROTOCOL.md §9.5's rationale against turning a 50ms
        // wake into a multi-second one.
        if (!skip_sleep && st.mqtt_connected) {
            msg_pump();
        }

        // F4: checkComm() every 60 wake cycles in sleep mode.
        if (g_rtc.mode == (uint8_t) PAGER_MODE_SLEEP &&
            (wake_cycle_count % PAGER_CHECKCOMM_EVERY_N_WAKES) == 0) {
            run_modem_health_check();
        }

        maybe_publish_heartbeat();

        // Wake source #4: active -> sleep after 10 min idle. Part A bug #1
        // fix: compare against the monotonic deadline, not wall-clock
        // epoch (see active_until_us's field comment above).
        if (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE) {
            if (esp_timer_get_time() >= g_rtc.active_until_us) {
                set_mode(PAGER_MODE_SLEEP, MODE_REASON_IDLE_TIMEOUT);
            }
        }

        rtc_lock();
        rtc_save();
        rtc_unlock();
    }
}

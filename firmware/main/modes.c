// modes.c — mode state machine, RTC memory contract, wake-and-drain loop.
//
// Authority: docs/PROTOCOL.md §4.1 (ack rules), §5.4 (status cadence), §8
// (wake sources), §9 (RTC memory), §11 (mode state machine stub).
//
// modes.c is the only place that touches the RTC struct and the only place
// that changes `mode` (funnelled through set_mode(), per §11). msg.c/ui.c
// stay Phase 5 stubs: incoming messages are logged here, not rendered or
// acked yet.
//
// All power-effect comments are PENDING_HW.

#include "modes.h"
#include "net.h"
#include "pins.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

// F1/F3 backoff schedule: 5s, 15s, 60s, 300s, then steady at 300s.
static const uint32_t k_backoff_s[] = { 5, 15, 60, 300 };
#define PAGER_BACKOFF_STEPS (sizeof(k_backoff_s) / sizeof(k_backoff_s[0]))

// ---------------------------------------------------------------------------
// RTC memory contract (PROTOCOL.md §9). Sized so the field-by-field totals
// line up with the table there (seen_ids ~196B, pending_acks ~128B,
// pending_up ~1.4kB, msg_ring ~3.6kB); see modes_boot()'s sizeof() log for
// the actual number, since the walter-modem library keeps its own
// RTC-resident state in the same 8kB region.
// ---------------------------------------------------------------------------

#define PAGER_ID_MAX_LEN 17     // 16 chars + NUL, PROTOCOL.md §1/§3.1
#define PAGER_FROM_MAX_LEN 17   // 16 chars + NUL
#define PAGER_SEEN_IDS_MAX 16    // §4.1 rule 7 - PROTOCOL.md says do not shrink this one

// PROTOCOL.md §9's sizing table (msg_ring/pending_up holding up to the full
// 320 UTF-8 byte body, §3.1) assumes the walter-modem library consumes
// "1-2kB" of the 8kB RTC_SLOW region. Measured against the actual v1.5.0
// checkout here (all four protocols enabled - see sdkconfig.defaults for
// why none of them can be safely disabled), the library alone consumes
// ~7.0kB (PDP + MQTT-topic + socket + BlueCherry RTC state), leaving only
// ~1.2kB total for everything in this struct, seen_ids included. That is
// roughly 6x worse than §9's estimate and makes the full-body sizing in the
// doc's own table (~5.4kB) physically impossible to link.
//
// This is a real discrepancy from the documented budget, not a Phase 4
// design choice - flagged in the phase report for firmware-architect
// re-review of PROTOCOL.md §9. Until that happens, pending_up/msg_ring
// (both explicitly Phase 5's to populate; msg.c is still a stub) are sized
// down to a short display-snippet body and a small depth purely so this
// phase links; Phase 5 MUST revisit both the depths and PAGER_RTC_SNIPPET_
// BYTES below against whatever body-truncation policy PROTOCOL.md ends up
// choosing (its own §9 already names "truncate to 160 bytes" as the first
// lever - even that does not fit today's measured budget).
#define PAGER_RTC_SNIPPET_BYTES 48 // PENDING PROTOCOL.md re-review, see above
#define PAGER_PENDING_ACKS_MAX 8   // §4.1 rule 6 - ids only, small, keeps spec's size
#define PAGER_PENDING_UP_MAX 2     // PROTOCOL.md §9 wants 4; reduced, see note above
#define PAGER_MSG_RING_MAX 3       // PROTOCOL.md §9 wants 10; reduced, see note above

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
    char id[PAGER_ID_MAX_LEN];
} pager_seen_id_t;

typedef struct {
    bool in_use;
    char id[PAGER_ID_MAX_LEN];
    uint8_t state; // 0=pending "shown", 1=pending "read"
    uint8_t attempts;
} pager_pending_ack_t;

typedef struct {
    bool in_use;
    char id[PAGER_ID_MAX_LEN];
    int64_t ts;
    char body[PAGER_RTC_SNIPPET_BYTES]; // truncated snippet, see the RTC budget note above
    uint8_t attempts;
} pager_pending_up_t;

typedef struct {
    bool in_use;
    char id[PAGER_ID_MAX_LEN];
    int64_t ts;
    char from[PAGER_FROM_MAX_LEN];
    char body[PAGER_RTC_SNIPPET_BYTES]; // truncated snippet, see the RTC budget note above
    uint8_t flags;
} pager_msg_ring_entry_t;

// magic encodes both validity and a layout version tag (per CLAUDE.md's RTC
// convention: "a version tag and a CRC"). Bump the trailing digit if the
// struct layout below changes shape.
#define PAGER_RTC_MAGIC 0x50475231u // "PGR" + layout version 1

typedef struct {
    uint32_t magic;
    uint32_t crc32;

    uint32_t boot_count;
    char session_id[12]; // "s_" + 8 hex + NUL, PROTOCOL.md §1

    uint8_t mode; // pager_mode_t
    int64_t active_until_epoch;

    uint32_t status_pub_count;
    int64_t last_status_epoch;

    uint32_t seen_head;
    pager_seen_id_t seen_ids[PAGER_SEEN_IDS_MAX];

    pager_pending_ack_t pending_acks[PAGER_PENDING_ACKS_MAX];
    pager_pending_up_t pending_up[PAGER_PENDING_UP_MAX];
    pager_msg_ring_entry_t msg_ring[PAGER_MSG_RING_MAX];

    uint32_t mqtt_memfull_count; // §8.4 M6, cumulative
    uint32_t oversize_drop_count; // F6, cumulative

    uint32_t modem_resets;         // F4, cumulative
    int64_t last_modem_reset_us;   // esp_timer_get_time() at last reset, for the 10min rate limit

    uint32_t attach_fail_cycles; // F1
    uint32_t wake_cycle_count;   // drives the "every 60 wakes" F4 check
} pager_rtc_t;

RTC_DATA_ATTR static pager_rtc_t g_rtc;

// Guards g_rtc against the (rare, Phase-4-scaffolding-only) race between the
// wake-and-drain loop below and the MQTT MESSAGE callback, which runs on
// WalterModem's _eventProcessingTask and currently only calls set_mode().
// Statically allocated: no heap use after init, per the house rule.
static StaticSemaphore_t s_rtc_mutex_buf;
static SemaphoreHandle_t s_rtc_mutex;

static void rtc_lock(void) { xSemaphoreTake(s_rtc_mutex, portMAX_DELAY); }
static void rtc_unlock(void) { xSemaphoreGive(s_rtc_mutex); }

// Phase 5 hook (PROTOCOL.md §4 "composer-open carve-out"): when true, the
// wake-and-drain loop skips net_sleep() entirely so ui.c can poll the CardKB
// at 100ms. Never set in Phase 4 - msg.c/ui.c are still stubs.
static bool s_composer_open = false;

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
    if ((pager_mode_t) g_rtc.mode == new_mode) {
        rtc_unlock();
        return;
    }
    ESP_LOGI(TAG, "mode: %s -> %s (reason=%d)", mode_name((pager_mode_t) g_rtc.mode),
             mode_name(new_mode), (int) reason);
    g_rtc.mode = (uint8_t) new_mode;
    if (new_mode == PAGER_MODE_ACTIVE) {
        g_rtc.active_until_epoch = approx_epoch() + PAGER_ACTIVE_IDLE_TIMEOUT_S;
    }
    rtc_save();
    rtc_unlock();

    // §5.4(b): publish on every mode change, but only if the session is
    // actually usable - otherwise this just becomes a queued publish that
    // races the next connect's own online announcement.
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (st.mqtt_connected) {
        publish_status_online();
    }
}

// ---------------------------------------------------------------------------
// Incoming message hook. msg.c's dedup/ack/render state machine is Phase 5;
// this phase only proves the plumbing from net.c's event handler through to
// a mode transition.
// ---------------------------------------------------------------------------

static void on_incoming_message(const char *topic, const char *body, uint16_t len)
{
    (void) body;
    ESP_LOGI(TAG, "incoming message on '%s' (%u bytes) - msg.c rendering is Phase 5", topic,
             (unsigned) len);
    set_mode(PAGER_MODE_ACTIVE, MODE_REASON_INCOMING_MSG);
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
// Button wake (#2 in the wake source table)
// ---------------------------------------------------------------------------

static void handle_button_wake(void)
{
    // Distinguishing short (mark read / open composer) vs long (send reply)
    // press needs hold-time measurement, which needs ui.c/CardKB (Phase 5).
    // For Phase 4, any button wake enters active mode.
    ESP_LOGD(TAG, "button wake (IO%d)", PAGER_PIN_BUTTON);
    set_mode(PAGER_MODE_ACTIVE, MODE_REASON_BUTTON);
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void modes_boot(void)
{
    s_rtc_mutex = xSemaphoreCreateMutexStatic(&s_rtc_mutex_buf);

    if (rtc_is_valid()) {
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

    ESP_LOGI(TAG, "pager_rtc_t size = %u bytes (8kB RTC slow-memory budget, PROTOCOL.md §9; "
                  "the walter-modem library keeps its own state in the same region)",
             (unsigned) sizeof(g_rtc));

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

        if (!s_composer_open) {
            net_sleep(interval_ms);
        } else {
            // Phase 5 hook: composer polls CardKB at 100ms instead of sleeping.
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // L4/F7: the event task ticks at 10ms + settles for 10ms; give it
        // >=30ms of awake time before looking at anything it may have
        // produced.
        vTaskDelay(pdMS_TO_TICKS(PAGER_POST_WAKE_YIELD_MS));
        assert(PAGER_POST_WAKE_YIELD_MS >= 30); // F7, debug builds only

        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            handle_button_wake();
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

        // F4: checkComm() every 60 wake cycles in sleep mode.
        if (g_rtc.mode == (uint8_t) PAGER_MODE_SLEEP &&
            (wake_cycle_count % PAGER_CHECKCOMM_EVERY_N_WAKES) == 0) {
            run_modem_health_check();
        }

        maybe_publish_heartbeat();

        // Wake source #4: active -> sleep after 10 min idle.
        if (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE) {
            if (approx_epoch() != 0 && approx_epoch() >= g_rtc.active_until_epoch) {
                set_mode(PAGER_MODE_SLEEP, MODE_REASON_IDLE_TIMEOUT);
            }
        }

        rtc_lock();
        rtc_save();
        rtc_unlock();
    }
}

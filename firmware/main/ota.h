/* ota.h — streaming OTA firmware update (docs/OTA_DESIGN.md).
 *
 * Split the usual way: everything above the `#ifdef ESP_PLATFORM` banner is
 * pure C (host-tested by firmware/host/test_ota.c): the `cfg.ota` sub-map
 * decode, the accept/reject decision, the "may a download start now" gate
 * (D6), the budget/expiry rules, the install gate and the state/error/UI
 * strings. Below the banner: the NVS job record, the partition handling and
 * the one-step-per-call driver (ota_service()) that ties cafetch's body sink
 * to otapipe and esp_ota_*.
 *
 * Power: nothing here runs while the job is parked (airplane, low battery,
 * recent input); the download is a second TLS socket on the cafetch profile,
 * driven from modes_run() like the CA and book fetches.
 */
#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cafetch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_ID_MAX 17            /* message id: 16 chars + NUL (PROTOCOL.md section 1) */
#define OTA_URL_MAX (CAFETCH_HOST_MAX + CAFETCH_PATH_MAX)
#define OTA_IMG_MAX 0x200000u    /* largest image or object the job may name (2 MiB) */

#define OTA_FMT_FULL 0
#define OTA_FMT_DELTA 1

/* D6 numbers. */
#define OTA_MIN_BATT_MV 3600
#define OTA_MIN_RSSI (-105)
#define OTA_MIN_INPUT_AGE_MS 60000u            /* no key in the last 60 s */
#define OTA_ATTEMPT_SPACING_US ((int64_t) 600 * 1000000) /* one attempt per 10 min */
#define OTA_MAX_ATTEMPTS_DAY 6
#define OTA_INSTALL_IDLE_MS (5u * 60u * 1000u) /* 5 min without input, sleep mode */
#define OTA_JOB_MAX_AGE_S (7u * 86400u)
#define OTA_BUDGET_EXTRA 65536u                /* budget = 3 * osz + 64 KiB (section 6) */
#define OTA_CONN_COST 7168u                    /* bytes charged per connection: handshake + headers + overhead */

/* ---- cfg.ota sub-map (OTA_KEYMAP img 0, isz 1, url 2, osz 3, osha 4, fmt 5, base 6, psz 7, cancel 8) */
typedef struct {
    uint8_t img[32];
    uint32_t isz;
    char url[OTA_URL_MAX];
    uint32_t osz;
    uint8_t osha[32];
    uint8_t fmt;          /* OTA_FMT_* */
    uint8_t base[32];
    uint32_t psz;
    bool cancel;
} ota_cfg_t;

/* Decodes the sub-map. False on: a missing required key, a hash that is not
 * 32 bytes, a URL that is not https://, isz/osz 0 or > OTA_IMG_MAX, a delta
 * without base/psz, or an unknown fmt. Unknown keys are skipped. A map with
 * `cancel:true` needs no other key (out->cancel = true). */
bool ota_parse_cfg_submap(const uint8_t *cbor, uint16_t len, ota_cfg_t *out);

typedef enum {
    OTA_ACCEPT_OK = 0,
    OTA_REJECT_RUNNING, /* img is already the running image */
    OTA_REJECT_BASE,    /* delta whose base is not the running image */
    OTA_REJECT_PV,      /* the running image is itself still pending verify */
    OTA_REJECT_NOBL,    /* no rollback-capable bootloader */
    OTA_REJECT_BAD,     /* img is on the failed list */
} ota_reject_t;

/* `bad` is NULL when there is no failed image on record. */
ota_reject_t ota_check_accept(const ota_cfg_t *cfg, const uint8_t running[32], bool pending_verify,
                              bool rollback_ok, const uint8_t bad[32]);

/* ---- job record ---- */
typedef enum {
    OTA_ST_NONE = 0,
    OTA_ST_WAIT,
    OTA_ST_DL,
    OTA_ST_READY,
    OTA_ST_INST,
    OTA_ST_OK,
    OTA_ST_FAIL,
    OTA_ST_RB,
} ota_state_t;

typedef enum {
    OTA_ERR_NONE = 0,
    OTA_ERR_BASE,
    OTA_ERR_HASH,
    OTA_ERR_OSHA,
    OTA_ERR_HTTP,
    OTA_ERR_BUDGET,
    OTA_ERR_FLASH,
    OTA_ERR_NOBL,
    OTA_ERR_PV,
    OTA_ERR_SIZE,
    OTA_ERR_EXPIRED,
    OTA_ERR_MEM,
    OTA_ERR_RUN,
    OTA_ERR_BAD,
} ota_err_t;

typedef struct {
    ota_cfg_t cfg;
    char id[OTA_ID_MAX];       /* the cfg message id, for the `shown` ack */
    uint8_t state;             /* ota_state_t */
    uint8_t err;               /* ota_err_t */
    uint8_t fail_hash_count;   /* hash/osha failures so far (one retry from 0 is allowed) */
    uint8_t attempts_day;
    uint32_t day;              /* epoch / 86400 that attempts_day counts */
    uint32_t created_epoch;    /* 0 = clock was unknown */
    uint32_t bytes_spent;      /* on-air estimate across every attempt */
    int64_t last_attempt_us;   /* RAM only (zeroed on load): esp_timer time of the last start, 0 = none */
} ota_job_t;

/* Everything the start/install gates look at; filled by modes.c each call. */
typedef struct {
    bool airplane;
    bool mqtt_usable;
    bool other_fetch;          /* a CA or book fetch is in flight */
    bool pending_verify;       /* the running image is not yet marked valid */
    bool sleep_mode;           /* g_rtc.mode == sleep (install gate) */
    int batt_mv;
    int rssi;                  /* dBm; 0 = unknown (passes) */
    uint32_t input_age_ms;     /* ms since the last key */
    int64_t now_us;
    uint32_t epoch;            /* UTC seconds, 0 = unknown */
} ota_env_t;

/* 3 * osz + 64 KiB. */
uint32_t ota_budget(const ota_cfg_t *cfg);
bool ota_budget_exceeded(const ota_job_t *j);
/* True 7 days after created_epoch (only when epoch > 0 and created_epoch > 0). */
bool ota_expired(const ota_env_t *e, const ota_job_t *j);

/* D6: state is WAIT, not airplane, MQTT usable, no other fetch, not pending
 * verify, battery >= 3600 mV, rssi >= -105, input at least 60 s old, at least
 * 10 min since the last attempt, fewer than 6 attempts today, and neither
 * budget nor expiry hit. */
bool ota_should_start(const ota_env_t *e, const ota_job_t *j);

/* Reboot into the new image: `install_now` (the Device screen), or sleep mode
 * with at least 5 min without input. */
bool ota_should_install(const ota_env_t *e, bool sleep_mode, bool install_now);

/* Strings exactly as OTA_DESIGN.md section 5: wait dl ready inst ok fail rb;
 * errors base hash osha http budget flash nobl pv size expired (+ mem run bad).
 * NONE gives "". */
const char *ota_state_str(ota_state_t s);
const char *ota_err_str(ota_err_t e);

/* 16 lowercase hex chars of the first 8 bytes (the short id). */
void ota_hex16(const uint8_t id[32], char out[17]);

/* Device-screen line (without the "Update: " label). */
void ota_ui_text(ota_state_t s, ota_err_t err, unsigned pct, char *out, size_t cap);

/* Maps a rejection to the error code shown in `ota_err`. */
ota_err_t ota_reject_err(ota_reject_t r);

#ifdef ESP_PLATFORM

/* The status fields modes.c sends (STK_OTA_*). */
typedef struct {
    char img[OTA_ID_MAX];      /* running image short id, always present once ota_boot() ran */
    bool gate;                 /* `ota:1`: rollback bootloader confirmed */
    bool have_job;             /* send ota_t / ota_st / ota_pct / ota_err */
    char target[OTA_ID_MAX];
    ota_state_t state;
    ota_err_t err;
    uint8_t pct;
} ota_status_t;

typedef void (*ota_lock_fn)(void);
typedef void (*ota_unlock_fn)(void);

/* Same cross-task binding catrust_bind() uses. Call before ota_boot(). */
void ota_bind(ota_lock_fn lock, ota_unlock_fn unlock);

/* Reads the running image id and rollback state, settles a job left by the
 * previous boot (ok / rb / nobl). NVS and a flash SHA over ~700 KB (logged).
 * Call after the first frame has been drawn. */
void ota_boot(void);

/* Records a cfg.ota (MQTT event task): decide accept/reject, ack `shown`, stash
 * the result for ota_service(). No modem or sleep effect. */
void ota_apply_cfg_submap(const uint8_t *cbor, uint16_t len, const char *id);

/* One step per call from modes_run(): drain, poll the socket, start, finish or
 * install. Power: one TLS socket while downloading, nothing otherwise. */
void ota_service(const ota_env_t *env);

/* True from the start of a download until it ends: keeps the loop awake. */
bool ota_transfer_active(void);

/* modes.c calls this after every successful /status publish (rollback
 * confirmation). */
void ota_on_status_published(bool mqtt_connected);

void ota_get_status(ota_status_t *out);

/* modes.c calls this when a /status it just built carries an `ok` or `rb` outcome: the next
 * successful publish then retires the job record ("one status after it ends"). */
void ota_status_included(void);

/* `ready`: the Device screen offers "Install now". */
bool ota_install_ready(void);
void ota_install_now(void);

/* Set by ota_service() when it wants a /status published (dl start, 50 %,
 * ready/fail/rb); modes.c polls and clears it. */
bool ota_take_status_request(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */

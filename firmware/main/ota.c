/* ota.c — see ota.h. */
#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cbor.h"

/* ---------------------------------------------------------------------
 * cfg.ota decode.
 * --------------------------------------------------------------------- */

#define OK_IMG 0
#define OK_ISZ 1
#define OK_URL 2
#define OK_OSZ 3
#define OK_OSHA 4
#define OK_FMT 5
#define OK_BASE 6
#define OK_PSZ 7
#define OK_CANCEL 8

static bool read_hash(cbor_r_t *r, uint8_t out[32])
{
    const uint8_t *b;
    size_t n;
    if (!cbor_r_bstr(r, &b, &n) || n != 32) {
        return false;
    }
    memcpy(out, b, 32);
    return true;
}

bool ota_parse_cfg_submap(const uint8_t *cbor, uint16_t len, ota_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    cbor_r_t r;
    cbor_r_init(&r, cbor, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count)) {
        return false;
    }
    uint32_t have = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        uint64_t u;
        switch (key) {
        case OK_IMG:
            if (!read_hash(&r, out->img)) {
                return false;
            }
            have |= 1u << OK_IMG;
            break;
        case OK_ISZ:
            if (!cbor_r_uint(&r, &u) || u == 0 || u > OTA_IMG_MAX) {
                return false;
            }
            out->isz = (uint32_t) u;
            have |= 1u << OK_ISZ;
            break;
        case OK_URL: {
            const char *s;
            size_t n;
            if (!cbor_r_tstr(&r, &s, &n) || n >= sizeof(out->url) || n < 8 || memcmp(s, "https://", 8) != 0) {
                return false;
            }
            memcpy(out->url, s, n);
            out->url[n] = '\0';
            have |= 1u << OK_URL;
            break;
        }
        case OK_OSZ:
            if (!cbor_r_uint(&r, &u) || u == 0 || u > OTA_IMG_MAX) {
                return false;
            }
            out->osz = (uint32_t) u;
            have |= 1u << OK_OSZ;
            break;
        case OK_OSHA:
            if (!read_hash(&r, out->osha)) {
                return false;
            }
            have |= 1u << OK_OSHA;
            break;
        case OK_FMT:
            if (!cbor_r_uint(&r, &u) || (u != OTA_FMT_FULL && u != OTA_FMT_DELTA)) {
                return false;
            }
            out->fmt = (uint8_t) u;
            have |= 1u << OK_FMT;
            break;
        case OK_BASE:
            if (!read_hash(&r, out->base)) {
                return false;
            }
            have |= 1u << OK_BASE;
            break;
        case OK_PSZ:
            if (!cbor_r_uint(&r, &u) || u == 0 || u > 0xFFFFFFFFu) {
                return false;
            }
            out->psz = (uint32_t) u;
            have |= 1u << OK_PSZ;
            break;
        case OK_CANCEL: {
            bool b;
            if (!cbor_r_bool(&r, &b)) {
                return false;
            }
            out->cancel = b;
            if (b) {
                have |= 1u << OK_CANCEL;
            }
            break;
        }
        default:
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }
    if (out->cancel) {
        return true; /* nothing else is needed to abort a job */
    }
    const uint32_t required = (1u << OK_IMG) | (1u << OK_ISZ) | (1u << OK_URL) | (1u << OK_OSZ) |
                              (1u << OK_OSHA) | (1u << OK_FMT);
    if ((have & required) != required) {
        return false;
    }
    if (out->fmt == OTA_FMT_DELTA && !((have & (1u << OK_BASE)) && (have & (1u << OK_PSZ)))) {
        return false;
    }
    return true;
}

ota_reject_t ota_check_accept(const ota_cfg_t *cfg, const uint8_t running[32], bool pending_verify,
                              bool rollback_ok, const uint8_t bad[32])
{
    if (memcmp(cfg->img, running, 32) == 0) {
        return OTA_REJECT_RUNNING;
    }
    if (bad && memcmp(cfg->img, bad, 32) == 0) {
        return OTA_REJECT_BAD;
    }
    if (pending_verify) {
        return OTA_REJECT_PV;
    }
    if (!rollback_ok) {
        return OTA_REJECT_NOBL;
    }
    if (cfg->fmt == OTA_FMT_DELTA && memcmp(cfg->base, running, 32) != 0) {
        return OTA_REJECT_BASE;
    }
    return OTA_ACCEPT_OK;
}

ota_err_t ota_reject_err(ota_reject_t r)
{
    switch (r) {
    case OTA_REJECT_RUNNING:
        return OTA_ERR_RUN;
    case OTA_REJECT_BASE:
        return OTA_ERR_BASE;
    case OTA_REJECT_PV:
        return OTA_ERR_PV;
    case OTA_REJECT_NOBL:
        return OTA_ERR_NOBL;
    case OTA_REJECT_BAD:
        return OTA_ERR_BAD;
    default:
        return OTA_ERR_NONE;
    }
}

/* ---------------------------------------------------------------------
 * Gates (D6).
 * --------------------------------------------------------------------- */

uint32_t ota_budget(const ota_cfg_t *cfg)
{
    return 3u * cfg->osz + OTA_BUDGET_EXTRA;
}

bool ota_budget_exceeded(const ota_job_t *j)
{
    return j->bytes_spent > ota_budget(&j->cfg);
}

bool ota_expired(const ota_env_t *e, const ota_job_t *j)
{
    if (e->epoch == 0 || j->created_epoch == 0) {
        return false;
    }
    return e->epoch > j->created_epoch && (e->epoch - j->created_epoch) > OTA_JOB_MAX_AGE_S;
}

bool ota_should_start(const ota_env_t *e, const ota_job_t *j)
{
    if (j->state != OTA_ST_WAIT) {
        return false;
    }
    if (e->airplane || !e->mqtt_usable || e->other_fetch || e->pending_verify) {
        return false;
    }
    if (e->batt_mv < OTA_MIN_BATT_MV || e->rssi < OTA_MIN_RSSI) {
        return false;
    }
    if (e->input_age_ms < OTA_MIN_INPUT_AGE_MS) {
        return false;
    }
    if (j->last_attempt_us != 0 && e->now_us - j->last_attempt_us < OTA_ATTEMPT_SPACING_US) {
        return false;
    }
    uint32_t today = e->epoch / 86400u;
    if (j->day == today && j->attempts_day >= OTA_MAX_ATTEMPTS_DAY) {
        return false;
    }
    if (ota_budget_exceeded(j) || ota_expired(e, j)) {
        return false;
    }
    return true;
}

bool ota_should_install(const ota_env_t *e, bool sleep_mode, bool install_now)
{
    if (install_now) {
        return true;
    }
    return sleep_mode && e->input_age_ms >= OTA_INSTALL_IDLE_MS;
}

/* ---------------------------------------------------------------------
 * Strings.
 * --------------------------------------------------------------------- */

const char *ota_state_str(ota_state_t s)
{
    switch (s) {
    case OTA_ST_WAIT:
        return "wait";
    case OTA_ST_DL:
        return "dl";
    case OTA_ST_READY:
        return "ready";
    case OTA_ST_INST:
        return "inst";
    case OTA_ST_OK:
        return "ok";
    case OTA_ST_FAIL:
        return "fail";
    case OTA_ST_RB:
        return "rb";
    default:
        return "";
    }
}

const char *ota_err_str(ota_err_t e)
{
    switch (e) {
    case OTA_ERR_BASE:
        return "base";
    case OTA_ERR_HASH:
        return "hash";
    case OTA_ERR_OSHA:
        return "osha";
    case OTA_ERR_HTTP:
        return "http";
    case OTA_ERR_BUDGET:
        return "budget";
    case OTA_ERR_FLASH:
        return "flash";
    case OTA_ERR_NOBL:
        return "nobl";
    case OTA_ERR_PV:
        return "pv";
    case OTA_ERR_SIZE:
        return "size";
    case OTA_ERR_EXPIRED:
        return "expired";
    case OTA_ERR_MEM:
        return "mem";
    case OTA_ERR_RUN:
        return "run";
    case OTA_ERR_BAD:
        return "bad";
    default:
        return "";
    }
}

void ota_hex16(const uint8_t id[32], char out[17])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[2 * i] = hex[id[i] >> 4];
        out[2 * i + 1] = hex[id[i] & 15];
    }
    out[16] = '\0';
}

void ota_ui_text(ota_state_t s, ota_err_t err, unsigned pct, char *out, size_t cap)
{
    switch (s) {
    case OTA_ST_WAIT:
        snprintf(out, cap, "waiting");
        break;
    case OTA_ST_DL:
        snprintf(out, cap, "downloading %u%%", pct > 100 ? 100u : pct);
        break;
    case OTA_ST_READY:
        snprintf(out, cap, "ready, installs when idle");
        break;
    case OTA_ST_INST:
        snprintf(out, cap, "installing");
        break;
    case OTA_ST_OK:
        snprintf(out, cap, "updated");
        break;
    case OTA_ST_FAIL:
        snprintf(out, cap, "failed (%s)", ota_err_str(err));
        break;
    case OTA_ST_RB:
        snprintf(out, cap, "rolled back");
        break;
    default:
        snprintf(out, cap, "-");
        break;
    }
}

/* ======================================================================
 * Device section: NVS job, partitions, one-step-per-call driver.
 * ====================================================================== */
#ifdef ESP_PLATFORM

#include "cafetch.h"
#include "modes.h"
#include "msg.h"
#include "otapipe.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define OTA_NVS_NS "ota"
#define OTA_JOB_VER 1
#define OTA_DRAIN_CAP 16384            /* inflated bytes per call: 4 sector erases, ~230 ms worst case */
#define OTA_MAX_RESUMES 5
#define OTA_PERSIST_EVERY 65536u
#define OTA_VALID_AIRPLANE_US ((int64_t) 120 * 1000000)
#define OTA_VALID_DEADLINE_US ((int64_t) 15 * 60 * 1000000)

typedef struct {
    uint32_t ver;
    uint32_t size;
    ota_job_t job;
} job_blob_t;

/* Cross-task binding (same pattern as catrust_bind()). */
static void lock_noop(void) {}
static ota_lock_fn s_lock = lock_noop;
static ota_unlock_fn s_unlock = lock_noop;

void ota_bind(ota_lock_fn lock, ota_unlock_fn unlock)
{
    s_lock = lock;
    s_unlock = unlock;
}

/* Boot facts. */
static bool s_booted = false;
static uint8_t s_running[32];
static bool s_pending_verify = false;
static bool s_valid_done = false;
static int64_t s_boot_us = 0;
static bool s_nobl = false;
static bool s_have_bad = false;
static uint8_t s_bad[32];

/* The job (modes_run task writes; ota_get_status() may read from another task under s_lock). */
static bool s_have_job = false;
static ota_job_t s_job;
static uint32_t s_unsaved_bytes = 0;

/* cfg.ota handoff (MQTT event task -> modes_run task), guarded by s_lock. */
static bool s_pend_present = false;
static ota_cfg_t s_pend_cfg;
static char s_pend_id[OTA_ID_MAX];

/* Flags from other tasks. */
static volatile bool s_mark_valid_req = false;
static volatile bool s_install_now = false;
static volatile bool s_status_req = false;
static volatile bool s_end_inflight = false; /* an ok/rb status was handed out; clear the job once it is published */
static volatile bool s_clear_req = false;    /* set by ota_on_status_published(), consumed by ota_service() */

/* Transfer (modes_run task only). */
static otapipe_t *s_pipe = NULL;
static volatile bool s_transfer_active = false;
static esp_ota_handle_t s_ota_handle = 0;
static bool s_ota_open = false;
static const esp_partition_t *s_target = NULL;
static esp_partition_mmap_handle_t s_mmap = 0;
static bool s_mmapped = false;
static bool s_cf_open = false;       /* we own the cafetch single-flight slot */
static bool s_fetch_done = false;
static bool s_resume_pending = false;
static bool s_write_err = false;
static uint32_t s_in_total = 0;      /* object bytes handed to the pipe */
static uint8_t s_resumes = 0;
static bool s_pub50 = false;

static uint32_t pct_now(void)
{
    uint32_t osz = s_job.cfg.osz;
    return osz ? (uint32_t) (((uint64_t) s_in_total * 100u) / osz) : 0;
}

/* ---- NVS ---- */

static void job_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (!s_have_job) {
        nvs_erase_key(h, "job");
    } else {
        job_blob_t b;
        memset(&b, 0, sizeof(b));
        b.ver = OTA_JOB_VER;
        b.size = sizeof(b);
        b.job = s_job;
        nvs_set_blob(h, "job", &b, sizeof(b));
    }
    nvs_commit(h);
    nvs_close(h);
    s_unsaved_bytes = 0;
}

static void job_load(void)
{
    s_have_job = false;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    job_blob_t b;
    size_t n = sizeof(b);
    if (nvs_get_blob(h, "job", &b, &n) == ESP_OK && n == sizeof(b) && b.ver == OTA_JOB_VER &&
        b.size == sizeof(b)) {
        s_job = b.job;
        s_job.last_attempt_us = 0; /* RAM-only field */
        s_have_job = s_job.state != OTA_ST_NONE;
    }
    nvs_close(h);
}

static void bad_persist(const uint8_t id[32])
{
    memcpy(s_bad, id, 32);
    s_have_bad = true;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bad", id, 32);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* `nobl` records the running image id at the time it was detected (blob, not a bare u8): a USB
 * reflash of bootloader + a different app then re-arms the gate by itself (see ota_boot()). */
static void nobl_set(bool on)
{
    s_nobl = on;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (on) {
        nvs_set_blob(h, "nobl", s_running, 32);
    } else {
        nvs_erase_key(h, "nobl");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void set_state(ota_state_t st, ota_err_t err)
{
    s_lock();
    s_job.state = (uint8_t) st;
    s_job.err = (uint8_t) err;
    s_unlock();
}

static void clear_job(void)
{
    s_lock();
    s_have_job = false;
    memset(&s_job, 0, sizeof(s_job));
    s_unlock();
    job_persist();
}

/* ---- boot ---- */

static void mark_valid(const char *why)
{
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s (%s)", e == ESP_OK ? "ok" : "failed", why);
    s_pending_verify = false;
    s_valid_done = true;
}

void ota_boot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    int64_t t0 = esp_timer_get_time();
    if (run == NULL || esp_partition_get_sha256(run, s_running) != ESP_OK) {
        ESP_LOGI(TAG, "cannot read the running image id; OTA disabled this boot");
        memset(s_running, 0, sizeof(s_running));
        return;
    }
    ESP_LOGI(TAG, "running image id computed in %lld ms", (long long) ((esp_timer_get_time() - t0) / 1000));

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    s_pending_verify = (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY);
    s_boot_us = esp_timer_get_time();

    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t nb[32];
        size_t n = sizeof(nb);
        if (nvs_get_blob(h, "nobl", nb, &n) == ESP_OK && n == 32) {
            s_nobl = true;
            if (memcmp(nb, s_running, 32) != 0 || s_pending_verify) {
                s_nobl = false; /* a different image is running (reflashed), or rollback is demonstrably live */
            }
        }
        n = sizeof(s_bad);
        s_have_bad = nvs_get_blob(h, "bad", s_bad, &n) == ESP_OK && n == 32;
        nvs_close(h);
        if (!s_nobl) {
            nobl_set(false); /* no-op write when the key was never there */
        }
    }

    job_load();
    char id16[17];
    ota_hex16(s_running, id16);
    ESP_LOGI(TAG, "running img=%s state=%d pending_verify=%d nobl=%d job=%s", id16, (int) st,
             (int) s_pending_verify, (int) s_nobl, s_have_job ? ota_state_str((ota_state_t) s_job.state) : "none");

    if (s_have_job) {
        bool is_target = memcmp(s_job.cfg.img, s_running, 32) == 0;
        switch ((ota_state_t) s_job.state) {
        case OTA_ST_INST:
        case OTA_ST_READY:
            if (is_target) {
                if (s_pending_verify) {
                    set_state(OTA_ST_OK, OTA_ERR_NONE);
                    ESP_LOGI(TAG, "update to %s booted; waiting for the first /status to confirm it", id16);
                } else {
                    /* rollback not live: the bootloader was not updated */
                    nobl_set(true);
                    mark_valid("no rollback bootloader");
                    set_state(OTA_ST_OK, OTA_ERR_NOBL);
                    ESP_LOGI(TAG, "update booted but the bootloader has no rollback support: gate closed");
                }
            } else if (esp_ota_get_last_invalid_partition() != NULL) {
                bad_persist(s_job.cfg.img);
                set_state(OTA_ST_RB, OTA_ERR_NONE);
                ESP_LOGI(TAG, "rolled back from the update: img marked bad");
            } else {
                /* The reboot never happened (or the boot partition never switched): download again. */
                set_state(OTA_ST_WAIT, OTA_ERR_NONE);
            }
            break;
        case OTA_ST_DL:
            set_state(OTA_ST_WAIT, OTA_ERR_NONE); /* power lost mid-download: restart from 0 next window */
            break;
        default:
            if (is_target && s_job.state != OTA_ST_OK && s_job.state != OTA_ST_RB) {
                s_have_job = false; /* already running the target (USB flash): job is obsolete */
                memset(&s_job, 0, sizeof(s_job));
            }
            break;
        }
        job_persist();
    }
    s_booted = true;
}

/* ---- cfg.ota (event task) ---- */

void ota_apply_cfg_submap(const uint8_t *cbor, uint16_t len, const char *id)
{
    ota_cfg_t cfg;
    if (!ota_parse_cfg_submap(cbor, len, &cfg)) {
        /* A malformed job is dropped without an ack: the relay re-publishes, and an operator sees it
         * in the log. (Rejection reasons that are about state, not shape, DO ack.) */
        ESP_LOGI(TAG, "malformed cfg.ota sub-map dropped (id=%s)", id ? id : "");
        return;
    }
    s_lock();
    s_pend_cfg = cfg;
    strncpy(s_pend_id, id ? id : "", sizeof(s_pend_id) - 1);
    s_pend_id[sizeof(s_pend_id) - 1] = '\0';
    s_pend_present = true;
    s_unlock();
    ESP_LOGI(TAG, "cfg.ota recorded (%s), id=%s - ota_service() decides", cfg.cancel ? "cancel" : "job",
             id ? id : "");
}

/* ---- transfer plumbing (modes_run task) ---- */

static void teardown_transfer(void)
{
    if (s_cf_open) {
        cafetch_end(); /* power effect: one AT command, closes the TLS socket */
        s_cf_open = false;
    }
    if (s_ota_open) {
        esp_ota_abort(s_ota_handle);
        s_ota_open = false;
    }
    if (s_pipe) {
        otapipe_deinit(s_pipe);
        heap_caps_free(s_pipe);
        s_pipe = NULL;
    }
    if (s_mmapped) {
        esp_partition_munmap(s_mmap);
        s_mmapped = false;
    }
    s_transfer_active = false;
    s_fetch_done = false;
    s_resume_pending = false;
}

/* A READY job already switched the boot partition to the new image; undo that when the job is
 * cancelled or replaced, so the next reboot stays on the running image. */
static void revert_ready_boot(void)
{
    if (s_have_job && s_job.state == OTA_ST_READY) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (run && esp_ota_set_boot_partition(run) == ESP_OK) {
            ESP_LOGI(TAG, "boot partition set back to the running image");
        }
    }
}

static void request_status(void)
{
    s_status_req = true;
}

static void fail_job(ota_err_t err)
{
    teardown_transfer();
    if ((err == OTA_ERR_HASH || err == OTA_ERR_OSHA) && s_job.fail_hash_count < 1) {
        s_job.fail_hash_count++;
        set_state(OTA_ST_WAIT, err); /* one retry from 0 in the next window */
        ESP_LOGI(TAG, "ota %s: retrying once from 0 in the next window", ota_err_str(err));
    } else {
        set_state(OTA_ST_FAIL, err);
        ESP_LOGI(TAG, "ota failed: %s", ota_err_str(err));
    }
    job_persist();
    request_status();
}

static void park_job(const char *why)
{
    teardown_transfer();
    set_state(OTA_ST_WAIT, OTA_ERR_NONE);
    job_persist();
    ESP_LOGI(TAG, "download parked (%s)", why);
}

static bool sink(void *arg, const uint8_t *data, size_t len)
{
    (void) arg;
    if (!otapipe_push(s_pipe, data, len)) {
        return false;
    }
    s_in_total += (uint32_t) len;
    s_job.bytes_spent += (uint32_t) len;
    s_unsaved_bytes += (uint32_t) len;
    return true;
}

static int write_cb(void *arg, const uint8_t *buf, size_t len)
{
    (void) arg;
    if (esp_ota_write(s_ota_handle, buf, len) != ESP_OK) {
        s_write_err = true;
        return -1;
    }
    return 0;
}

static bool open_connection(void)
{
    s_job.bytes_spent += OTA_CONN_COST;
    s_unsaved_bytes += OTA_CONN_COST;
    /* Power effect: one TLS handshake on the cafetch profile, radio up for the transfer. */
    if (!cafetch_begin_stream(s_job.cfg.url, s_in_total, s_job.cfg.osz - s_in_total, sink, NULL)) {
        return false;
    }
    s_cf_open = true;
    return true;
}

static void start_transfer(const ota_env_t *env)
{
    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL || s_target->size < s_job.cfg.isz) {
        fail_job(OTA_ERR_SIZE);
        return;
    }
    if (s_job.cfg.fmt == OTA_FMT_DELTA && memcmp(s_job.cfg.base, s_running, 32) != 0) {
        fail_job(OTA_ERR_BASE);
        return;
    }
    s_pipe = (otapipe_t *) heap_caps_malloc(otapipe_sizeof(), MALLOC_CAP_SPIRAM);
    if (!s_pipe) {
        fail_job(OTA_ERR_MEM);
        return;
    }
    const uint8_t *base = NULL;
    size_t base_len = 0;
    if (s_job.cfg.fmt == OTA_FMT_DELTA) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        const void *p = NULL;
        if (esp_partition_mmap(run, 0, run->size, ESP_PARTITION_MMAP_DATA, &p, &s_mmap) != ESP_OK) {
            fail_job(OTA_ERR_MEM);
            return;
        }
        s_mmapped = true;
        base = (const uint8_t *) p;
        base_len = run->size;
    }
    s_write_err = false;
    if (esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle) != ESP_OK) {
        fail_job(OTA_ERR_FLASH);
        return;
    }
    s_ota_open = true;
    bool delta = s_job.cfg.fmt == OTA_FMT_DELTA;
    if (otapipe_init(s_pipe, delta, delta ? s_job.cfg.psz : s_job.cfg.isz, base, base_len, write_cb, NULL) != 0) {
        fail_job(OTA_ERR_MEM);
        return;
    }
    otapipe_set_out_limit(s_pipe, s_job.cfg.isz);

    uint32_t today = env->epoch / 86400u;
    if (s_job.day != today) {
        s_job.day = today;
        s_job.attempts_day = 0;
    }
    s_job.attempts_day++;
    s_job.last_attempt_us = env->now_us;
    s_in_total = 0;
    s_resumes = 0;
    s_pub50 = false;
    s_fetch_done = false;
    s_resume_pending = false;
    s_transfer_active = true;
    set_state(OTA_ST_DL, OTA_ERR_NONE);
    char t[17];
    ota_hex16(s_job.cfg.img, t);
    ESP_LOGI(TAG, "download start: target=%s fmt=%s object=%u B attempt %u today", t,
             delta ? "delta" : "full", (unsigned) s_job.cfg.osz, (unsigned) s_job.attempts_day);
    if (!open_connection()) {
        ESP_LOGI(TAG, "could not open the download connection; will retry in a later window");
        teardown_transfer();
        set_state(OTA_ST_WAIT, OTA_ERR_HTTP);
        job_persist();
        return;
    }
    job_persist();
    request_status();
}

static void complete_transfer(void)
{
    uint8_t osha[32];
    uint32_t out_len = 0;
    if (otapipe_finish(s_pipe, osha, &out_len) < 0) {
        fail_job(s_write_err ? OTA_ERR_FLASH : OTA_ERR_HASH);
        return;
    }
    if (memcmp(osha, s_job.cfg.osha, 32) != 0) {
        fail_job(OTA_ERR_OSHA);
        return;
    }
    if (out_len != s_job.cfg.isz) {
        fail_job(OTA_ERR_HASH);
        return;
    }
    esp_err_t e = esp_ota_end(s_ota_handle); /* re-verifies the image (appended digest) */
    s_ota_open = false;
    if (e != ESP_OK) {
        ESP_LOGI(TAG, "esp_ota_end failed: %s", esp_err_to_name(e));
        fail_job(OTA_ERR_HASH);
        return;
    }
    uint8_t got[32];
    if (esp_partition_get_sha256(s_target, got) != ESP_OK || memcmp(got, s_job.cfg.img, 32) != 0) {
        fail_job(OTA_ERR_HASH);
        return;
    }
    if (esp_ota_set_boot_partition(s_target) != ESP_OK) {
        fail_job(OTA_ERR_FLASH);
        return;
    }
    teardown_transfer();
    set_state(OTA_ST_READY, OTA_ERR_NONE);
    job_persist();
    ESP_LOGI(TAG, "update downloaded and verified (%u B on air est.); boot partition switched, waiting to install",
             (unsigned) s_job.bytes_spent);
    request_status();
}

static void drain_step(void)
{
    int r = otapipe_drain(s_pipe, OTA_DRAIN_CAP);
    if (r < 0) {
        ESP_LOGI(TAG, "decode error %d", r);
        fail_job(s_write_err ? OTA_ERR_FLASH : OTA_ERR_HASH);
        return;
    }
    if (r == 1) {
        complete_transfer();
        return;
    }
    if (!s_pub50 && pct_now() >= 50) {
        s_pub50 = true;
        request_status();
    }
    if (s_unsaved_bytes >= OTA_PERSIST_EVERY) {
        job_persist();
    }
}

static void transfer_step(const ota_env_t *env)
{
    if (env->airplane) {
        park_job("airplane mode");
        return;
    }
    /* (a) input waiting: inflate/patch/write one bounded slice. */
    if (otapipe_pending(s_pipe)) {
        drain_step();
        return;
    }
    /* Fetch finished and everything consumed: the final drain is what returns 1. */
    if (s_fetch_done) {
        drain_step();
        return;
    }
    /* (b) resume after a broken transfer. */
    if (s_resume_pending) {
        s_resume_pending = false;
        if (!open_connection()) {
            if (++s_resumes > OTA_MAX_RESUMES) {
                fail_job(OTA_ERR_HTTP);
            } else {
                s_resume_pending = true;
            }
        }
        return;
    }
    /* (b) poll the socket; the sink pushes into the pipe. */
    cafetch_status_t cs = cafetch_poll(env->now_us);
    if (cs == CAFETCH_PENDING) {
        return;
    }
    if (cs == CAFETCH_OK) {
        cafetch_end();
        s_cf_open = false;
        s_fetch_done = true;
        otapipe_set_eof(s_pipe);
        return;
    }
    /* CAFETCH_FAILED */
    int status = 0;
    size_t got = 0;
    bool transport = false;
    cafetch_fail_info(&status, &got, &transport, NULL);
    cafetch_end();
    s_cf_open = false;
    if (s_in_total >= s_job.cfg.osz) {
        s_fetch_done = true; /* everything had arrived before the connection broke */
        otapipe_set_eof(s_pipe);
        return;
    }
    if (transport && s_resumes < OTA_MAX_RESUMES) {
        s_resumes++;
        s_resume_pending = true;
        ESP_LOGI(TAG, "transfer broke at %u/%u B (resume %u/%u)", (unsigned) s_in_total,
                 (unsigned) s_job.cfg.osz, (unsigned) s_resumes, (unsigned) OTA_MAX_RESUMES);
        return;
    }
    ESP_LOGI(TAG, "download failed: http=%d transport=%d at %u B", status, (int) transport, (unsigned) s_in_total);
    fail_job(OTA_ERR_HTTP);
}

static void do_install(void)
{
    set_state(OTA_ST_INST, OTA_ERR_NONE);
    job_persist();
    ESP_LOGI(TAG, "installing: rebooting into the new image");
    modes_publish_status_now(); /* power effect: one publish (if connected) before the reboot */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void take_pending(const ota_env_t *env)
{
    s_lock();
    bool present = s_pend_present;
    ota_cfg_t cfg = s_pend_cfg;
    char id[OTA_ID_MAX];
    strncpy(id, s_pend_id, sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    if (present) {
        s_pend_present = false;
    }
    s_unlock();
    if (!present) {
        return;
    }

    if (cfg.cancel) {
        ESP_LOGI(TAG, "cancel: job dropped");
        teardown_transfer();
        s_install_now = false;
        revert_ready_boot();
        if (s_have_job && s_job.state != OTA_ST_INST) {
            clear_job();
        }
        msg_mark_shown(id);
        return;
    }
    /* The same target pushed again while it is still live is a duplicate delivery: ack only. */
    if (s_have_job && memcmp(s_job.cfg.img, cfg.img, 32) == 0 &&
        (s_job.state == OTA_ST_WAIT || s_job.state == OTA_ST_DL || s_job.state == OTA_ST_READY ||
         s_job.state == OTA_ST_INST)) {
        msg_mark_shown(id);
        return;
    }
    teardown_transfer(); /* a newer cfg.ota replaces the job */
    s_install_now = false;
    revert_ready_boot();
    ota_reject_t rj = ota_check_accept(&cfg, s_running, s_pending_verify, !s_nobl,
                                       s_have_bad ? s_bad : NULL);
#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (rj == OTA_ACCEPT_OK) {
        rj = OTA_REJECT_NOBL;
    }
#endif
    ota_err_t err = ota_reject_err(rj);
    if (rj == OTA_ACCEPT_OK) {
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (next == NULL || next->size < cfg.isz) {
            err = OTA_ERR_SIZE;
        }
    }
    s_lock();
    memset(&s_job, 0, sizeof(s_job));
    s_job.cfg = cfg;
    strncpy(s_job.id, id, sizeof(s_job.id) - 1);
    s_job.created_epoch = env->epoch;
    s_job.state = (uint8_t) (err == OTA_ERR_NONE ? OTA_ST_WAIT : OTA_ST_FAIL);
    s_job.err = (uint8_t) err;
    s_have_job = true;
    s_unlock();
    job_persist();
    msg_mark_shown(id);
    char t[17];
    ota_hex16(cfg.img, t);
    ESP_LOGI(TAG, "cfg.ota %s: target=%s fmt=%s %s%s", err == OTA_ERR_NONE ? "accepted" : "rejected", t,
             cfg.fmt == OTA_FMT_DELTA ? "delta" : "full", err == OTA_ERR_NONE ? "" : "err=", ota_err_str(err));
    if (err != OTA_ERR_NONE) {
        request_status();
    }
}

static void validity_tick(const ota_env_t *env)
{
    if (s_mark_valid_req && s_pending_verify && !s_valid_done) {
        s_mark_valid_req = false;
        mark_valid("first /status published on a connected session");
        return;
    }
    s_mark_valid_req = false;
    if (!s_pending_verify || s_valid_done || s_boot_us == 0) {
        return;
    }
    int64_t up = env->now_us - s_boot_us;
    if (env->airplane) {
        if (up >= OTA_VALID_AIRPLANE_US) {
            mark_valid("airplane mode, 120 s of healthy loop");
        }
    } else if (up >= OTA_VALID_DEADLINE_US) {
        ESP_LOGI(TAG, "new image not confirmed after 15 min: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot(); /* reboots; returns only on error */
    }
}

void ota_service(const ota_env_t *env_in)
{
    if (!s_booted) {
        return;
    }
    ota_env_t e = *env_in;
    e.pending_verify = s_pending_verify; /* ota.c owns this fact */
    const ota_env_t *env = &e;
    take_pending(env);
    validity_tick(env);
    if (s_clear_req) {
        s_clear_req = false;
        if (s_have_job && (s_job.state == OTA_ST_OK || s_job.state == OTA_ST_RB)) {
            clear_job();
        }
    }
    if (!s_have_job) {
        return;
    }
    if (s_pipe) {
        transfer_step(env);
        return;
    }
    switch ((ota_state_t) s_job.state) {
    case OTA_ST_WAIT:
        if (ota_budget_exceeded(&s_job)) {
            fail_job(OTA_ERR_BUDGET);
        } else if (ota_expired(env, &s_job)) {
            fail_job(OTA_ERR_EXPIRED);
        } else if (ota_should_start(env, &s_job)) {
            start_transfer(env);
        }
        break;
    case OTA_ST_READY:
        if (ota_should_install(env, env->sleep_mode, s_install_now)) {
            do_install();
        }
        break;
    default:
        break;
    }
}

bool ota_transfer_active(void)
{
    return s_transfer_active;
}

void ota_on_status_published(bool mqtt_connected)
{
    if (mqtt_connected && s_pending_verify && !s_valid_done) {
        s_mark_valid_req = true; /* marked from ota_service() (modes_run task), not from this event task */
    }
    if (s_end_inflight) {
        /* The status that carried an ok/rb outcome is out: ota_service() drops the job record. */
        s_end_inflight = false;
        s_clear_req = true;
    }
}

void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_booted) {
        ota_hex16(s_running, out->img);
    }
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    out->gate = s_booted && !s_nobl;
#endif
    s_lock();
    if (s_have_job && s_job.state != OTA_ST_NONE) {
        out->have_job = true;
        ota_hex16(s_job.cfg.img, out->target);
        out->state = (ota_state_t) s_job.state;
        out->err = (ota_err_t) s_job.err;
        out->pct = (out->state == OTA_ST_READY || out->state == OTA_ST_INST || out->state == OTA_ST_OK)
            ? 100
            : (uint8_t) (s_transfer_active ? pct_now() : 0);
    }
    s_unlock();
}

void ota_status_included(void)
{
    s_end_inflight = true;
}

bool ota_install_ready(void)
{
    return s_have_job && s_job.state == OTA_ST_READY;
}

void ota_install_now(void)
{
    s_install_now = true;
}

bool ota_take_status_request(void)
{
    bool r = s_status_req;
    s_status_req = false;
    return r;
}

#endif /* ESP_PLATFORM */

/* refreshpol.c -- see refreshpol.h. Pure core first, device half below. */
#include "refreshpol.h"

void refreshpol_init(refreshpol_t *p)
{
    p->dirty = 0;
    p->last_key_us = 0;
    p->last_partial_us = 0;
    p->key_seen = false;
    p->floor = PAGER_REFRESH_FLOOR;
    p->ceiling = PAGER_REFRESH_CEILING;
    p->idle_s = PAGER_REFRESH_IDLE_S;
    p->gap_ms = PAGER_REFRESH_GAP_MS;
    p->presleep_min = PAGER_REFRESH_PRESLEEP_MIN;
    p->reason = REFRESHPOL_REASON_OTHER;
}

void refreshpol_on_key(refreshpol_t *p, int64_t now_us)
{
    p->last_key_us = now_us;
    p->key_seen = true;
}

void refreshpol_on_partial(refreshpol_t *p, int64_t now_us)
{
    p->dirty++;
    p->last_partial_us = now_us;
}

void refreshpol_on_full(refreshpol_t *p)
{
    p->dirty = 0;
    p->reason = REFRESHPOL_REASON_OTHER;
}

static bool grant(refreshpol_t *p, refreshpol_reason_t r)
{
    p->reason = r;
    return true;
}

bool refreshpol_want_full(refreshpol_t *p, int64_t now_us, bool transition, bool pre_sleep)
{
    p->reason = REFRESHPOL_REASON_OTHER;
    if (pre_sleep) {
        return (p->dirty > 0 && p->dirty >= p->presleep_min) ? grant(p, REFRESHPOL_REASON_PRESLEEP) : false;
    }
    if (transition && p->dirty >= p->floor) {
        return grant(p, REFRESHPOL_REASON_TRANSITION);
    }
    int64_t since_key_us = p->key_seen ? (now_us - p->last_key_us) : INT64_MAX;
    if (since_key_us < (int64_t) p->gap_ms * 1000) {
        return false; /* mid-burst: never */
    }
    if (p->dirty >= p->ceiling) {
        return grant(p, REFRESHPOL_REASON_CEILING);
    }
    if (p->dirty >= p->floor && since_key_us >= (int64_t) p->idle_s * 1000000) {
        return grant(p, REFRESHPOL_REASON_IDLE);
    }
    return false;
}

const char *refreshpol_reason_str(refreshpol_reason_t r)
{
    switch (r) {
    case REFRESHPOL_REASON_IDLE: return "idle";
    case REFRESHPOL_REASON_TRANSITION: return "transition";
    case REFRESHPOL_REASON_PRESLEEP: return "presleep";
    case REFRESHPOL_REASON_CEILING: return "ceiling";
    default: return "other";
    }
}

/* ======================= device half ======================= */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "refreshpol";
static refreshpol_t s_pol;
static bool s_pol_ready = false;

refreshpol_t *refreshpol_global(void)
{
    if (!s_pol_ready) {
        refreshpol_init(&s_pol);
        s_pol_ready = true;
    }
    return &s_pol;
}

void refreshpol_load_nvs(void)
{
    refreshpol_t *p = refreshpol_global();
    nvs_handle_t h;
    if (nvs_open("disp", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t v;
    if (nvs_get_u32(h, "floor", &v) == ESP_OK) p->floor = v;
    if (nvs_get_u32(h, "ceil", &v) == ESP_OK) p->ceiling = v;
    if (nvs_get_u32(h, "idle", &v) == ESP_OK) p->idle_s = v;
    if (nvs_get_u32(h, "gap", &v) == ESP_OK) p->gap_ms = v;
    if (nvs_get_u32(h, "psmin", &v) == ESP_OK) p->presleep_min = v;
    nvs_close(h);
    ESP_LOGI(TAG, "knobs floor=%u ceil=%u idle=%us gap=%ums psmin=%u", (unsigned) p->floor,
             (unsigned) p->ceiling, (unsigned) p->idle_s, (unsigned) p->gap_ms,
             (unsigned) p->presleep_min);
}

// Power effect: one NVS (flash) write, negligible.
void refreshpol_save_nvs(void)
{
    refreshpol_t *p = refreshpol_global();
    nvs_handle_t h;
    if (nvs_open("disp", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "floor", p->floor);
    nvs_set_u32(h, "ceil", p->ceiling);
    nvs_set_u32(h, "idle", p->idle_s);
    nvs_set_u32(h, "gap", p->gap_ms);
    nvs_set_u32(h, "psmin", p->presleep_min);
    nvs_commit(h);
    nvs_close(h);
}

void refreshpol_note_key(void) { refreshpol_on_key(refreshpol_global(), esp_timer_get_time()); }

void refreshpol_note_partial(void) { refreshpol_on_partial(refreshpol_global(), esp_timer_get_time()); }

void refreshpol_note_full(void)
{
    refreshpol_t *p = refreshpol_global();
    ESP_LOGI(TAG, "full refresh: reason=%s dirty=%u", refreshpol_reason_str(p->reason),
             (unsigned) p->dirty);
    refreshpol_on_full(p);
}

bool refreshpol_poll(bool transition, bool pre_sleep)
{
    return refreshpol_want_full(refreshpol_global(), esp_timer_get_time(), transition, pre_sleep);
}
#endif /* ESP_PLATFORM */

/* ident.c — device identity, NVS namespace "ident". See ident.h.
 *
 * Authority: docs/DEVICE_PLAN.md §3.4, §2.7.
 */
#include "ident.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ident";
static const char *NVS_NS = "ident";

static ident_t s_ident;

/* docs/PROTOCOL.md §1: `^[a-z0-9][a-z0-9-]{2,23}$`, max len 24 (3..24
 * chars total: one leading [a-z0-9] plus 2..23 of [a-z0-9-]). */
static bool valid_dev_id(const char *s, size_t len)
{
    if (len < 3 || len > 24) {
        return false;
    }
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9'))) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Tri-state so the caller can tell "absent, use the default" apart from
 * "present but unreadable" (oversized or corrupt) without a second call. */
typedef enum {
    FIELD_OK,
    FIELD_MISSING,
    FIELD_ERROR,
} field_status_t;

static field_status_t read_str(nvs_handle_t h, const char *key, char *buf, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err == ESP_OK) {
        return FIELD_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return FIELD_MISSING;
    }
    ESP_LOGD(TAG, "key '%s' str read failed: 0x%x", key, err);
    buf[0] = '\0';
    return FIELD_ERROR;
}

static field_status_t read_blob(nvs_handle_t h, const char *key, void *buf, size_t exact_len)
{
    size_t len = exact_len;
    esp_err_t err = nvs_get_blob(h, key, buf, &len);
    if (err == ESP_OK && len == exact_len) {
        return FIELD_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(buf, 0, exact_len);
        return FIELD_MISSING;
    }
    ESP_LOGD(TAG, "key '%s' blob read failed: 0x%x len=%u", key, err, (unsigned)len);
    memset(buf, 0, exact_len);
    return FIELD_ERROR;
}

static field_status_t read_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    esp_err_t err = nvs_get_u8(h, key, out);
    if (err == ESP_OK) {
        return FIELD_OK;
    }
    *out = 0;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return FIELD_MISSING;
    }
    ESP_LOGD(TAG, "key '%s' u8 read failed: 0x%x", key, err);
    return FIELD_ERROR;
}

static field_status_t read_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    esp_err_t err = nvs_get_u16(h, key, out);
    if (err == ESP_OK) {
        return FIELD_OK;
    }
    *out = 0;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return FIELD_MISSING;
    }
    ESP_LOGD(TAG, "key '%s' u16 read failed: 0x%x", key, err);
    return FIELD_ERROR;
}

static field_status_t read_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    esp_err_t err = nvs_get_u32(h, key, out);
    if (err == ESP_OK) {
        return FIELD_OK;
    }
    *out = 0;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return FIELD_MISSING;
    }
    ESP_LOGD(TAG, "key '%s' u32 read failed: 0x%x", key, err);
    return FIELD_ERROR;
}

bool ident_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\") failed: 0x%x", NVS_NS, err);
        return false;
    }

    /* static, not a stack local: ident_t carries a 4 kB `ca` buffer and this
     * runs on the main task. As a local it overflowed that stack on the first
     * boot that actually had an identity in NVS (confirmed on hardware: reset
     * loop straight after the first successful setup). Boot is single-threaded
     * here, so a static scratch copy is safe. */
    static ident_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    /* Required: no identity without these (`ca` below is the exception). */
    bool ok = true;
    ok = ok && read_str(h, "dev_id", tmp.dev_id, sizeof(tmp.dev_id)) == FIELD_OK;
    ok = ok && read_str(h, "mqtt_pw", tmp.mqtt_pw, sizeof(tmp.mqtt_pw)) == FIELD_OK;
    ok = ok && read_blob(h, "kdev", tmp.kdev, sizeof(tmp.kdev)) == FIELD_OK;
    ok = ok && read_str(h, "host", tmp.host, sizeof(tmp.host)) == FIELD_OK;
    ok = ok && read_u16(h, "port", &tmp.port) == FIELD_OK;
    /* `ca` is optional: empty/absent means the production session pins no
     * CA (net_init() then uses validation off). Only a read error fails. */
    ok = ok && read_str(h, "ca", tmp.ca, sizeof(tmp.ca)) != FIELD_ERROR;

    if (ok && !valid_dev_id(tmp.dev_id, strlen(tmp.dev_id))) {
        ESP_LOGD(TAG, "dev_id '%s' fails PROTOCOL.md %%1 regex", tmp.dev_id);
        ok = false;
    }

    /* Optional, default to zero/empty when absent; a read error other
     * than "not found" still fails the whole load (corrupt/oversized
     * value is not something to silently paper over). */
    field_status_t st;

    st = read_str(h, "apn", tmp.apn, sizeof(tmp.apn));
    ok = ok && st != FIELD_ERROR;

    st = read_str(h, "label", tmp.label, sizeof(tmp.label));
    ok = ok && st != FIELD_ERROR;

    st = read_u32(h, "flags", &tmp.flags);
    ok = ok && st != FIELD_ERROR;

    st = read_blob(h, "ca_hash", tmp.ca_hash, sizeof(tmp.ca_hash));
    ok = ok && st != FIELD_ERROR;

    /* v0.2 (docs/V02_DESIGN.md §3): "n_epoch32" (u32) is the current key.
     * A v0.1 device only ever wrote "n_epoch" (u16, 12-bit-epoch era) —
     * read that instead, once, so an existing device's replay counter
     * continues from where it left off rather than silently resetting to 0
     * (which the relay would otherwise see as `n` going backwards / a
     * replay). ident_store() only ever writes "n_epoch32" from here on;
     * the next epoch bump (modes.c's on_auth_epoch_wrap()) persists the
     * migrated value there and the legacy key is simply never touched
     * again. */
    st = read_u32(h, "n_epoch32", &tmp.n_epoch);
    if (st == FIELD_MISSING) {
        uint16_t legacy_epoch = 0;
        field_status_t legacy_st = read_u16(h, "n_epoch", &legacy_epoch);
        ok = ok && legacy_st != FIELD_ERROR;
        tmp.n_epoch = legacy_epoch; /* 0 if legacy_st == FIELD_MISSING too */
    } else {
        ok = ok && st != FIELD_ERROR;
    }

    st = read_u8(h, "claimed", &tmp.claimed);
    ok = ok && st != FIELD_ERROR;

    nvs_close(h);

    if (!ok) {
        return false;
    }

    tmp.ca_len = strlen(tmp.ca);
    s_ident = tmp;
    return true;
}

bool ident_store(const ident_t *id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\", RW) failed: 0x%x", NVS_NS, err);
        return false;
    }

    bool ok = true;
    ok = ok && nvs_set_str(h, "dev_id", id->dev_id) == ESP_OK;
    ok = ok && nvs_set_str(h, "mqtt_pw", id->mqtt_pw) == ESP_OK;
    ok = ok && nvs_set_blob(h, "kdev", id->kdev, sizeof(id->kdev)) == ESP_OK;
    ok = ok && nvs_set_str(h, "host", id->host) == ESP_OK;
    ok = ok && nvs_set_u16(h, "port", id->port) == ESP_OK;
    ok = ok && nvs_set_str(h, "ca", id->ca) == ESP_OK;
    ok = ok && nvs_set_str(h, "apn", id->apn) == ESP_OK;
    ok = ok && nvs_set_u32(h, "flags", id->flags) == ESP_OK;
    ok = ok && nvs_set_str(h, "label", id->label) == ESP_OK;
    ok = ok && nvs_set_blob(h, "ca_hash", id->ca_hash, sizeof(id->ca_hash)) == ESP_OK;
    /* v0.2 (docs/V02_DESIGN.md §3): "n_epoch32" (u32) replaces the old
     * "n_epoch" (u16) key. The legacy key is simply left alone (unused,
     * harmless) rather than erased — ident_load()'s migration only reads it
     * when "n_epoch32" is absent. */
    ok = ok && nvs_set_u32(h, "n_epoch32", id->n_epoch) == ESP_OK;
    ok = ok && nvs_set_u8(h, "claimed", id->claimed) == ESP_OK;

    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);

    if (ok) {
        /* v0.2 bug fix #2 (docs/V02_DESIGN.md §2.2): keep the in-memory
         * copy current so ident_get_*() (e.g. ident_get_n_epoch(), read on
         * every signed publish) reflects what was just committed, not only
         * after the next reboot's ident_load(). Same ca_len derivation
         * ident_load() uses. */
        s_ident = *id;
        s_ident.ca_len = strlen(s_ident.ca);
    }
    return ok;
}

bool ident_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\", RW) failed: 0x%x", NVS_NS, err);
        return false;
    }
    bool ok = nvs_erase_all(h) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

const char *ident_get_dev_id(void) { return s_ident.dev_id; }
const char *ident_get_mqtt_pw(void) { return s_ident.mqtt_pw; }
const uint8_t *ident_get_kdev(void) { return s_ident.kdev; }
const char *ident_get_host(void) { return s_ident.host; }
uint16_t ident_get_port(void) { return s_ident.port; }
const char *ident_get_ca(void) { return s_ident.ca; }
size_t ident_get_ca_len(void) { return s_ident.ca_len; }
const char *ident_get_apn(void) { return s_ident.apn; }
uint32_t ident_get_flags(void) { return s_ident.flags; }
const char *ident_get_label(void) { return s_ident.label; }
const uint8_t *ident_get_ca_hash(void) { return s_ident.ca_hash; }
uint32_t ident_get_n_epoch(void) { return s_ident.n_epoch; }
uint8_t ident_get_claimed(void) { return s_ident.claimed; }

/* v0.2 §2.4: standalone NVS accessors for the "slot12" flag — deliberately
 * not folded into ident_t/ident_load()/ident_store() (which require a full
 * identity's worth of required fields to succeed): the bootstrap path calls
 * these before any identity exists. */
bool ident_get_slot12_populated(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "slot12", &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

bool ident_set_slot12_populated(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\", RW) failed: 0x%x", NVS_NS, err);
        return false;
    }
    bool ok = nvs_set_u8(h, "slot12", 1) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

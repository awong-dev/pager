// carrier.c — see carrier.h.
#include "carrier.h"

#include <string.h>

static const carrier_preset_t k_presets[] = {
    { "Automatic", "", NULL, "" },
    { "Carrier default", "", NULL, "" },
    // https://www.usmobile.com/help/docs/troubleshooting-and-setup/apn-settings-android-dark-star
    // ("MCC 310, MNC 410, or 280 if 410 fails to save; MVNO type GID, value 20FF").
    // Bench SIM 2026-09-20: EF_GID1 = 20FF, network reports mnc410.mcc310;
    // TLS MQTT in 5 s and the requested eDRX granted with this APN, neither with a blank one.
    { "US Mobile Dark Star", "ereseller", "310410 310280", "20FF" },
};

size_t carrier_preset_count(void) { return sizeof(k_presets) / sizeof(k_presets[0]); }

const carrier_preset_t *carrier_preset_at(size_t i)
{
    return (i < carrier_preset_count()) ? &k_presets[i] : NULL;
}

static bool alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool carrier_apn_valid(const char *apn)
{
    if (!apn) {
        return false;
    }
    size_t n = strlen(apn);
    if (n == 0) {
        return true;
    }
    if (n >= CARRIER_APN_MAX || !alnum(apn[0]) || !alnum(apn[n - 1])) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = apn[i];
        if (!(alnum(c) || c == '.' || c == '-')) {
            return false;
        }
        if (c == '.' && i + 1 < n && apn[i + 1] == '.') {
            return false;
        }
    }
    return true;
}

int carrier_preset_index_for(const char *apn)
{
    if (!apn) {
        return -1;
    }
    for (size_t i = 2; i < carrier_preset_count(); i++) {
        if (strcmp(k_presets[i].apn, apn) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static char up(char c) { return (c >= 'a' && c <= 'f') ? (char) (c - 'a' + 'A') : c; }

static bool plmn_listed(const char *plmns, const char *imsi)
{
    size_t ilen = strlen(imsi);
    const char *p = plmns;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *e = p;
        while (*e && *e != ' ') {
            e++;
        }
        size_t n = (size_t) (e - p);
        if (n >= 5 && n <= ilen && strncmp(p, imsi, n) == 0) {
            return true;
        }
        p = e;
    }
    return false;
}

const carrier_preset_t *carrier_detect(const char *imsi, const char *gid1_hex)
{
    if (!imsi || strlen(imsi) < 6) {
        return NULL;
    }
    if (!gid1_hex) {
        gid1_hex = "";
    }
    for (size_t i = 2; i < carrier_preset_count(); i++) {
        const carrier_preset_t *p = &k_presets[i];
        if (!p->plmns || !p->plmns[0] || !plmn_listed(p->plmns, imsi)) {
            continue;
        }
        size_t n = strlen(p->gid1_prefix);
        if (strlen(gid1_hex) < n) {
            continue;
        }
        bool match = true;
        for (size_t k = 0; k < n; k++) {
            if (up(gid1_hex[k]) != up(p->gid1_prefix[k])) {
                match = false;
                break;
            }
        }
        if (match) {
            return p;
        }
    }
    return NULL;
}

#ifdef ESP_PLATFORM

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "carrier";
static char s_apn[CARRIER_APN_MAX];
static carrier_mode_t s_mode = CARRIER_MODE_AUTO;
static bool s_loaded = false;
static char s_detected[32];

static void load_once(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    s_apn[0] = '\0';
    s_mode = CARRIER_MODE_AUTO;
    nvs_handle_t h;
    if (nvs_open("carrier", NVS_READONLY, &h) != ESP_OK) {
        return; /* never chosen: automatic */
    }
    uint8_t mode = 0;
    size_t len = sizeof(s_apn);
    if (nvs_get_u8(h, "mode", &mode) == ESP_OK && mode == (uint8_t) CARRIER_MODE_FIXED &&
        nvs_get_str(h, "apn", s_apn, &len) == ESP_OK && carrier_apn_valid(s_apn)) {
        s_mode = CARRIER_MODE_FIXED;
    } else {
        s_apn[0] = '\0';
    }
    nvs_close(h);
}

carrier_mode_t carrier_get_mode(void)
{
    load_once();
    return s_mode;
}

const char *carrier_get_apn(void)
{
    load_once();
    return s_apn;
}

const char *carrier_get_label(void)
{
    load_once();
    if (s_mode == CARRIER_MODE_AUTO) {
        return "Automatic";
    }
    if (s_apn[0] == '\0') {
        return "Carrier default";
    }
    int i = carrier_preset_index_for(s_apn);
    return (i >= 0) ? k_presets[i].label : "custom";
}

static bool persist(carrier_mode_t mode, const char *apn)
{
    nvs_handle_t h;
    if (nvs_open("carrier", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGI(TAG, "nvs_open(carrier) failed");
        return false;
    }
    // Power effect: one NVS write. Nothing touches the modem here; the new
    // choice is used by the next attach.
    bool ok = nvs_set_u8(h, "mode", (uint8_t) mode) == ESP_OK && nvs_set_str(h, "apn", apn) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        strncpy(s_apn, apn, sizeof(s_apn) - 1);
        s_apn[sizeof(s_apn) - 1] = '\0';
        s_mode = mode;
        s_loaded = true;
        ESP_LOGI(TAG, "carrier: %s (APN '%s'); used from the next attach", carrier_get_label(), s_apn);
    }
    return ok;
}

bool carrier_set_custom(const char *apn)
{
    return carrier_apn_valid(apn) && persist(CARRIER_MODE_FIXED, apn);
}

bool carrier_select_preset(size_t i)
{
    const carrier_preset_t *p = carrier_preset_at(i);
    if (!p) {
        return false;
    }
    return persist(i == CARRIER_PRESET_AUTO ? CARRIER_MODE_AUTO : CARRIER_MODE_FIXED, p->apn);
}

void carrier_note_detected(const char *label)
{
    strncpy(s_detected, label ? label : "", sizeof(s_detected) - 1);
    s_detected[sizeof(s_detected) - 1] = '\0';
}

const char *carrier_last_detected(void) { return s_detected; }

#endif /* ESP_PLATFORM */

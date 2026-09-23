// wificred.c — see wificred.h for the module split, the NVS key layout, and
// the hard "never a PSK near ESP_LOG" rule this whole file follows.

#include "wificred.h"

#include <string.h>

#include "cbor.h"

// ---------------------------------------------------------------------------
// Pure functions (no ESP-IDF dependency) — host-tested by
// firmware/host/test_wificred.c.
// ---------------------------------------------------------------------------

bool wificred_valid_ssid(const char *ssid, size_t len)
{
    if (!ssid || len == 0 || len > WIFICRED_SSID_MAX - 1) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (ssid[i] == '\0') {
            return false; // would silently truncate through nvs_set_str()/nvs_get_str()
        }
    }
    return true;
}

bool wificred_valid_psk(const char *psk, size_t len)
{
    if (!psk || len < 8 || len > WIFICRED_PSK_MAX - 1) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (psk[i] == '\0') {
            return false; // same nvs_set_str()/nvs_get_str() round-trip requirement as the SSID check
        }
    }
    return true;
}

bool wificred_apply_candidates(const wificred_candidate_t *candidates, uint8_t count, wificred_set_t *out)
{
    if (count > WIFICRED_MAX_NETS) {
        return false; // over the cap: reject the whole batch, never truncate
    }

    // Built into a scratch copy so a bad entry midway through never
    // partially applies, and a rejected batch never even touches the
    // caller's existing set -- same discipline sms_parse_cfg_submap()
    // documents.
    wificred_set_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    for (uint8_t i = 0; i < count; i++) {
        const wificred_candidate_t *c = &candidates[i];
        if (!wificred_valid_ssid(c->ssid, c->ssid_len) || !wificred_valid_psk(c->psk, c->psk_len)) {
            return false;
        }
        memcpy(tmp.nets[i].ssid, c->ssid, c->ssid_len);
        tmp.nets[i].ssid[c->ssid_len] = '\0';
        memcpy(tmp.nets[i].psk, c->psk, c->psk_len);
        tmp.nets[i].psk[c->psk_len] = '\0';
        tmp.nets[i].channel = 0; // a push never carries a channel -- learned only after association
    }
    tmp.count = count;

    *out = tmp;
    return true;
}

// PROTOCOL.md §10 `cfg.wifi` sub-map keys: en=0 (bool), nets=1 (array). Each
// `nets[]` item sub-map: s=0 (tstr, ssid), p=1 (tstr, psk).
#define WIFICK_EN 0
#define WIFICK_NETS 1
#define WIFI_NETK_S 0
#define WIFI_NETK_P 1

bool wificred_parse_cfg_submap(const uint8_t *buf, uint16_t len, wificred_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count)) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        switch (key) {
        case WIFICK_EN: {
            bool v;
            if (!cbor_r_bool(&r, &v)) {
                return false;
            }
            out->en = v;
            out->have_en = true;
            break;
        }
        case WIFICK_NETS: {
            uint32_t ncount;
            if (!cbor_r_array(&r, &ncount)) {
                return false;
            }
            if (ncount > WIFICRED_MAX_NETS) {
                return false; // over the cap: reject the whole push, never truncate
            }
            wificred_candidate_t cand[WIFICRED_MAX_NETS];
            for (uint32_t j = 0; j < ncount; j++) {
                uint32_t mcount;
                if (!cbor_r_map(&r, &mcount)) {
                    return false;
                }
                const char *s = NULL;
                size_t slen = 0;
                const char *p = NULL;
                size_t plen = 0;
                bool have_s = false, have_p = false;
                for (uint32_t k = 0; k < mcount; k++) {
                    uint32_t nk;
                    if (!cbor_r_key(&r, &nk)) {
                        return false;
                    }
                    switch (nk) {
                    case WIFI_NETK_S:
                        if (!cbor_r_tstr(&r, &s, &slen)) {
                            return false;
                        }
                        have_s = true;
                        break;
                    case WIFI_NETK_P:
                        if (!cbor_r_tstr(&r, &p, &plen)) {
                            return false;
                        }
                        have_p = true;
                        break;
                    default:
                        if (!cbor_r_skip(&r)) {
                            return false;
                        }
                        break;
                    }
                }
                if (!have_s || !have_p) {
                    return false; // both s and p are required in every network item
                }
                cand[j].ssid = s;
                cand[j].ssid_len = slen;
                cand[j].psk = p;
                cand[j].psk_len = plen;
            }
            // wificred_apply_candidates() re-validates every SSID/PSK
            // boundary and rejects the whole batch on the first bad entry --
            // this parser never duplicates that validation itself.
            if (!wificred_apply_candidates(cand, (uint8_t) ncount, &out->nets)) {
                return false;
            }
            out->have_nets = true; // true even for ncount==0 ("nets: []" clears the set)
            break;
        }
        default:
            // "unknown cfg keys must be skipped, not treated as malformed"
            // (cfg.c's own rule, applied one level down here for `wifi`'s
            // own sub-keys).
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }
    return true;
}

#ifdef ESP_PLATFORM

#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "wificred";
static const char *NVS_NS = "wifi";

static bool s_enabled = false;
static wificred_set_t s_set;

static void slot_keys(uint8_t idx, char *sk, size_t sk_cap, char *pk, size_t pk_cap, char *ck, size_t ck_cap)
{
    snprintf(sk, sk_cap, "s%u", (unsigned) idx);
    snprintf(pk, pk_cap, "p%u", (unsigned) idx);
    snprintf(ck, ck_cap, "ch%u", (unsigned) idx);
}

void wificred_init(void)
{
    s_enabled = false;
    memset(&s_set, 0, sizeof(s_set));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "wificred: no NVS namespace yet, no networks stored");
        return;
    }

    uint8_t en = 0;
    nvs_get_u8(h, "en", &en); // FIELD_MISSING -> en stays 0, same as "never enabled"
    s_enabled = (en != 0);

    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (n > WIFICRED_MAX_NETS) {
        n = WIFICRED_MAX_NETS; // defensive: corrupt/foreign NVS content must never overrun the set
    }

    uint8_t loaded = 0;
    for (uint8_t i = 0; i < n; i++) {
        char sk[5], pk[5], ck[6];
        slot_keys(i, sk, sizeof(sk), pk, sizeof(pk), ck, sizeof(ck));

        char ssid[WIFICRED_SSID_MAX] = "";
        char psk[WIFICRED_PSK_MAX] = "";
        size_t slen = sizeof(ssid);
        size_t plen = sizeof(psk);
        esp_err_t se = nvs_get_str(h, sk, ssid, &slen);
        esp_err_t pe = nvs_get_str(h, pk, psk, &plen);
        if (se != ESP_OK || pe != ESP_OK) {
            // Never trust a partially-written slot (a crash mid-store could
            // leave `n` ahead of what actually committed) -- stop here
            // rather than loading a network with a missing/corrupt secret.
            // Logs only the two esp_err_t codes, never a field's contents
            // (this file's own hard rule) -- and deliberately avoids even
            // the word for the secret field in the log text itself, so a
            // plain `grep -n psk` across this file has nothing to find.
            ESP_LOGI(TAG, "wificred: slot %u incomplete (name_err=0x%x pass_err=0x%x) - stopping load "
                          "at %u network(s)",
                     (unsigned) i, se, pe, (unsigned) loaded);
            break;
        }
        uint8_t ch = 0;
        nvs_get_u8(h, ck, &ch); // optional -- 0 (unknown) if never learned

        strncpy(s_set.nets[loaded].ssid, ssid, sizeof(s_set.nets[loaded].ssid) - 1);
        strncpy(s_set.nets[loaded].psk, psk, sizeof(s_set.nets[loaded].psk) - 1);
        s_set.nets[loaded].channel = ch;
        loaded++;
    }
    s_set.count = loaded;
    nvs_close(h);

    ESP_LOGI(TAG, "wificred: loaded %u network(s), enabled=%d", (unsigned) s_set.count, (int) s_enabled);
}

bool wificred_enabled(void) { return s_enabled; }

uint8_t wificred_count(void) { return s_set.count; }

bool wificred_get(uint8_t idx, wificred_net_t *out)
{
    if (idx >= s_set.count) {
        return false;
    }
    if (out) {
        *out = s_set.nets[idx];
    }
    return true;
}

bool wificred_set_enabled(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\", RW) failed: 0x%x", NVS_NS, err);
        return false;
    }
    bool ok = nvs_set_u8(h, "en", enabled ? 1 : 0) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    if (ok) {
        s_enabled = enabled;
        // Power effect: one NVS write only. No modem/RRC/WiFi-radio effect
        // of its own -- wifi_sta.c (a later task) is what actually
        // associates/disassociates in response to this flag changing.
        ESP_LOGI(TAG, "wificred: enabled -> %d", (int) enabled);
    }
    return ok;
}

bool wificred_store(const wificred_set_t *set)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(\"%s\", RW) failed: 0x%x", NVS_NS, err);
        return false;
    }

    uint8_t n = set->count > WIFICRED_MAX_NETS ? WIFICRED_MAX_NETS : set->count; // defensive
    bool ok = nvs_set_u8(h, "n", n) == ESP_OK;

    for (uint8_t i = 0; i < WIFICRED_MAX_NETS; i++) {
        char sk[5], pk[5], ck[6];
        slot_keys(i, sk, sizeof(sk), pk, sizeof(pk), ck, sizeof(ck));
        if (i < n) {
            ok = ok && nvs_set_str(h, sk, set->nets[i].ssid) == ESP_OK;
            ok = ok && nvs_set_str(h, pk, set->nets[i].psk) == ESP_OK;
            ok = ok && nvs_set_u8(h, ck, set->nets[i].channel) == ESP_OK;
        } else {
            // A shrinking set must never leave a stale, unreferenced PSK
            // sitting in flash -- erase, not merely overwrite with an empty
            // string. Harmless (ESP_ERR_NVS_NOT_FOUND) if the slot was
            // never written in the first place.
            nvs_erase_key(h, sk);
            nvs_erase_key(h, pk);
            nvs_erase_key(h, ck);
        }
    }

    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);

    if (ok) {
        s_set = *set;
        s_set.count = n;
        // Power effect: one NVS write only, no modem/RRC/WiFi-radio effect.
        // Never logs a PSK value (this file's own hard rule) -- SSIDs are
        // not secret, but are still left out of this log line to keep the
        // rule easy to verify by a single grep across the whole file.
        ESP_LOGI(TAG, "wificred: stored %u network(s)", (unsigned) s_set.count);
    }
    return ok;
}

bool wificred_clear(void)
{
    wificred_set_t empty;
    memset(&empty, 0, sizeof(empty));
    return wificred_store(&empty);
}

void wificred_note_channel(uint8_t idx, uint8_t channel)
{
    if (idx >= s_set.count) {
        return;
    }
    s_set.nets[idx].channel = channel;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return; // best-effort; the RAM cache above is already updated
    }
    char ck[6];
    snprintf(ck, sizeof(ck), "ch%u", (unsigned) idx);
    if (nvs_set_u8(h, ck, channel) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// `cfg.wifi` intercept (cfg.c's cfg_ingest_cbor(), docs/WIFI_TASKS.md W3).
// ---------------------------------------------------------------------------
#include "msg.h"

void wificred_apply_cfg_submap(const uint8_t *buf, uint16_t len, const char *id)
{
    wificred_cfg_t cfg;
    if (!wificred_parse_cfg_submap(buf, len, &cfg)) {
        ESP_LOGI(TAG, "malformed cfg.wifi sub-map dropped (id=%s)", id ? id : "");
        return;
    }

    if (cfg.have_en) {
        wificred_set_enabled(cfg.en);
    }
    if (cfg.have_nets) {
        wificred_store(&cfg.nets);
    }

    // §4: "nets absent -> apply en only, leave stored networks untouched" --
    // logged as counts/flags only, never a PSK (this file's own hard rule).
    ESP_LOGI(TAG, "cfg.wifi applied: en=%s nets=%s (id=%s)",
             cfg.have_en ? (cfg.en ? "on" : "off") : "unchanged",
             cfg.have_nets ? "replaced" : "unchanged", id ? id : "");
    if (id && id[0] != '\0') {
        // §4: apply-and-ack-`shown` immediately on success, exactly like
        // `cfg.sms` (cfg.c:161-165) -- no two-phase apply needed here.
        msg_mark_shown(id);
    }
}

#endif /* ESP_PLATFORM */

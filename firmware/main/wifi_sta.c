// wifi_sta.c — see wifi_sta.h for the module's own contract, and
// docs/WIFI_DESIGN.md §1/§4/§8 for the design this implements.

#include "wifi_sta.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_heap_caps.h"

#include "wificred.h"

static const char *TAG = "wifi_sta";

static bool s_started = false;
static esp_netif_t *s_netif = NULL;

static volatile bool s_associated = false;
static volatile bool s_got_ip = false;
static char s_ip_str[16] = "";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGD(TAG, "WIFI_EVENT_STA_START");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *) data;
            s_associated = true;
            ESP_LOGI(TAG, "wifi: associated (channel %u)", (unsigned) (e ? e->channel : 0));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *) data;
            s_associated = false;
            s_got_ip = false;
            s_ip_str[0] = '\0';
            ESP_LOGI(TAG, "wifi: disassociated (reason %u)", (unsigned) (e ? e->reason : 0));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
            esp_ip4addr_ntoa(&e->ip_info.ip, s_ip_str, sizeof(s_ip_str));
            s_got_ip = true;
            int rssi = 0;
            wifi_sta_get_rssi(&rssi);
            ESP_LOGI(TAG, "wifi: got ip %s rssi %d", s_ip_str, rssi);
        }
    }
}

bool wifi_sta_start(void)
{
    if (s_started) {
        return true;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "esp_netif_init() failed: 0x%x", err);
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "esp_event_loop_create_default() failed: 0x%x", err);
        return false;
    }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) {
            ESP_LOGI(TAG, "esp_netif_create_default_wifi_sta() failed");
            return false;
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_init() failed");
        return false;
    }

    // wifi_sta.h's own module comment: never mirror the PSK into esp_wifi's
    // own flash-backed config blob -- wificred's NVS namespace `wifi` is the
    // one place a PSK is ever persisted here.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          &wifi_event_handler, NULL, NULL));

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_set_mode(STA) failed");
        return false;
    }
    // docs/WIFI_DESIGN.md §8: MIN_MODEM is the active-mode default; the
    // net_sleep() WiFi branch (net.cpp) swaps to MAX_MODEM for the duration
    // of its own delay window only.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    // 13 dBm (52 in esp_wifi's quarter-dBm units) -- docs/WIFI_DESIGN.md §8:
    // a real lever against TX-current supply sag, and a home AP is close.
    esp_wifi_set_max_tx_power(52);

    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_start() failed");
        return false;
    }

    s_started = true;
    // The number the 2026-09-23 W6 bench was missing: mbedtls_ssl_setup()
    // needs ONE contiguous MBEDTLS_SSL_IN_CONTENT_LEN+~300 byte block, so the
    // largest free block matters more than the total. main.c's own "heap after
    // WiFi+TLS+MQTT up" line is only reached on the success path, which is
    // exactly the path that did not happen. Cost: one INFO line per `wifi on`.
    ESP_LOGI(TAG, "wifi: station started (WiFi driver up, not yet associated); "
                  "heap free=%u largest_8bit_block=%u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

void wifi_sta_stop(void)
{
    if (!s_started) {
        return;
    }
    esp_wifi_disconnect(); // best-effort; harmless if not associated
    esp_wifi_stop();
    s_started = false;
    s_associated = false;
    s_got_ip = false;
    s_ip_str[0] = '\0';
    ESP_LOGI(TAG, "wifi: station stopped");
}

bool wifi_sta_associate(void)
{
    if (!s_started) {
        ESP_LOGI(TAG, "wifi_sta_associate(): station not started");
        return false;
    }
    wificred_net_t net;
    if (!wificred_get(0, &net)) {
        ESP_LOGI(TAG, "wifi_sta_associate(): no credentials stored");
        return false;
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    // wifi_sta_config_t.ssid/.password are fixed-size, not required to be
    // NUL-terminated by esp_wifi itself, but wificred's own SSID_MAX/PSK_MAX
    // (33/64) already leave room for one and strncpy() below zero-pads the
    // rest -- never printed, never logged (wifi_sta.h's own module comment).
    strncpy((char *) cfg.sta.ssid, net.ssid, sizeof(cfg.sta.ssid));
    strncpy((char *) cfg.sta.password, net.psk, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // docs/WIFI_DESIGN.md §4: WPA2-PSK only
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    // docs/WIFI_DESIGN.md §8: used only once WIFI_PS_MAX_MODEM is active
    // (wifi_sta_set_ps_sleep()); harmless while WIFI_PS_MIN_MODEM (the
    // default) is in effect.
    cfg.sta.listen_interval = 10;
    if (net.channel != 0) {
        cfg.sta.channel = net.channel; // cached home channel: faster reassociate
    }

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_set_config(STA) failed");
        return false;
    }
    ESP_LOGI(TAG, "wifi: associating");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_connect() failed: 0x%x", err);
        return false;
    }
    return true;
}

void wifi_sta_disassociate(void)
{
    esp_wifi_disconnect();
}

bool wifi_sta_associated(void)
{
    return s_associated;
}

bool wifi_sta_got_ip(void)
{
    return s_got_ip;
}

bool wifi_sta_get_ip(char *out, size_t cap)
{
    if (!s_got_ip || !out || cap < sizeof(s_ip_str)) {
        return false;
    }
    strncpy(out, s_ip_str, cap);
    return true;
}

bool wifi_sta_get_rssi(int *dbm)
{
    if (!s_associated || !dbm) {
        return false;
    }
    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) != ESP_OK) {
        return false;
    }
    *dbm = rec.rssi;
    return true;
}

int wifi_sta_get_dtim_period(void)
{
    // See this function's own doc comment in wifi_sta.h: not exposed by
    // esp_wifi's public API on this IDF version.
    return -1;
}

void wifi_sta_set_ps_active(void)
{
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void wifi_sta_set_ps_sleep(void)
{
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

int wifi_sta_scan(wifi_sta_scan_result_t *out, int cap)
{
    if (!s_started || !out || cap <= 0) {
        return -1;
    }
    // Blocking active scan, default parameters (every channel, the driver's
    // own default per-channel dwell) -- diagnostic only (wifi_sta.h's own
    // module comment); W11's passive, single-cached-channel policy scan is a
    // different call this task does not add.
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_scan_start() failed");
        return -1;
    }
    uint16_t num = (uint16_t) cap;
    static wifi_ap_record_t recs[16]; // bounded scratch, no dynamic allocation
    if (num > (uint16_t) (sizeof(recs) / sizeof(recs[0]))) {
        num = (uint16_t) (sizeof(recs) / sizeof(recs[0]));
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
        return -1;
    }
    int n = (num < (uint16_t) cap) ? num : cap;
    for (int i = 0; i < n; i++) {
        memset(out[i].ssid, 0, sizeof(out[i].ssid));
        memcpy(out[i].ssid, recs[i].ssid, sizeof(recs[i].ssid) < sizeof(out[i].ssid) - 1
                                              ? sizeof(recs[i].ssid)
                                              : sizeof(out[i].ssid) - 1);
        out[i].rssi = recs[i].rssi;
        out[i].channel = recs[i].primary;
        out[i].authmode = (uint8_t) recs[i].authmode;
    }
    return n;
}

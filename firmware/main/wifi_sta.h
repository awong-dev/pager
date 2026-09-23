/* wifi_sta.h — ESP32-S3 WiFi station bring-up and association
 * (docs/WIFI_DESIGN.md §1, §4, §8; docs/WIFI_TASKS.md W5).
 *
 * Owns esp_netif/esp_wifi/the WIFI_EVENT+IP_EVENT handlers, association,
 * RSSI and IP-acquired state. Deliberately no MQTT here at all (docs/
 * WIFI_TASKS.md W5's own rule) — xport_wifi.c is the only caller that talks
 * esp-mqtt, and it does so over the netif this file brings up, never through
 * a function in this header.
 *
 * Credentials come from wificred.c (NVS namespace `wifi`, up to two
 * networks); this file only ever reads index 0 (docs/WIFI_TASKS.md W5: the
 * console's `wifi set` replaces the whole wificred set with a single entry,
 * so phase 1 never has to choose among several). Never logs a PSK — the
 * wifi_config_t built in wifi_sta_associate() holds one for as long as
 * esp_wifi needs it and this file never prints wifi_sta_config_t.password.
 *
 * PSK storage: esp_wifi_set_storage(WIFI_STORAGE_RAM) is set in
 * wifi_sta_start() so esp_wifi never mirrors the PSK into its own separate
 * NVS blob (the default is WIFI_STORAGE_FLASH) — wificred's NVS namespace
 * `wifi` is the one and only place a PSK is ever persisted on this device
 * (docs/WIFI_DESIGN.md §4's own "NVS is not encrypted" limit already covers
 * that one place; a second, un-audited copy in esp_wifi's own flash blob
 * would be a silent second exposure of the same limit).
 */
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* esp_netif_init() + esp_event_loop_create_default() (guarded: may already
 * exist, e.g. a future component that also needs the default loop) +
 * esp_netif_create_default_wifi_sta() + esp_wifi_init() + WIFI_EVENT/IP_EVENT
 * handler registration + esp_wifi_set_mode(WIFI_MODE_STA) +
 * esp_wifi_set_ps(WIFI_PS_MIN_MODEM) (docs/WIFI_DESIGN.md §8: active-mode
 * default) + esp_wifi_set_max_tx_power(52) (13 dBm, §8) + esp_wifi_start().
 * Idempotent (a static latch): safe to call from every `wifi on`. Does NOT
 * associate — call wifi_sta_associate() next.
 * Power effect: the WiFi PHY/MAC driver comes up and starts listening;
 * ESP-IDF's own ESP32-S3 figures (docs/WIFI_DESIGN.md §0) put an
 * associated station at 38-40 mA even before any MQTT traffic — this is the
 * point at which that floor starts costing current, station or not. */
bool wifi_sta_start(void);

/* esp_wifi_disconnect() (best-effort) + esp_wifi_stop() — the console `wifi
 * off` path (docs/WIFI_TASKS.md W5: "wifi off means use LTE"). Leaves
 * esp_wifi_init() itself in place (a `wifi on` right after just calls
 * esp_wifi_start() again, no second esp_wifi_init()/dynamic re-allocation).
 * Power effect: drops the association and stops the WiFi driver's own
 * tasks/radio -- current returns to whatever the LTE-only floor already was,
 * same as if WiFi had never been turned on this boot. */
void wifi_sta_stop(void);

/* Builds a wifi_sta_config_t from wificred_get(0, ...) (WPA2-PSK only,
 * threshold.authmode = WIFI_AUTH_WPA2_PSK per docs/WIFI_DESIGN.md §4;
 * listen_interval = 10, used only once WIFI_PS_MAX_MODEM is active --
 * wifi_sta_set_ps_sleep() below), esp_wifi_set_config(WIFI_IF_STA, ...) and
 * esp_wifi_connect(). Returns false without touching the radio if
 * wificred_get(0, ...) has nothing (no credentials stored) or wifi_sta_start()
 * has not been called yet. The caller (main.c's `wifi on`) polls
 * wifi_sta_got_ip() afterward -- this call only queues the association,
 * association/DHCP happen on the WiFi driver's own task and complete
 * asynchronously.
 * Power effect: one association handshake (probe/auth/assoc frames) plus
 * whatever DHCP needs -- a few hundred ms to a few seconds at the
 * already-associated current floor wifi_sta_start() started paying. */
bool wifi_sta_associate(void);

/* esp_wifi_disconnect(). Power effect: none of its own beyond the
 * disassociation frame; the station keeps drawing the wifi_sta_start()
 * floor until wifi_sta_stop() is also called. */
void wifi_sta_disassociate(void);

/* True from WIFI_EVENT_STA_CONNECTED until WIFI_EVENT_STA_DISCONNECTED (or
 * wifi_sta_stop()/wifi_sta_disassociate()). Association only -- says nothing
 * about whether an IP has been acquired yet (wifi_sta_got_ip() below) or
 * whether any MQTT session is up (net_get_mqtt_status(), a different
 * layer). */
bool wifi_sta_associated(void);

/* True from IP_EVENT_STA_GOT_IP until the next disconnect. xport_wifi.c's
 * caller (main.c's `wifi on`) waits for this, not just wifi_sta_associated(),
 * before calling net_xport_switch(NET_XPORT_WIFI) -- esp-mqtt needs a
 * working IP stack to dial the broker and disable_auto_reconnect=true means
 * a premature attempt just fails once rather than retrying (docs/
 * WIFI_DESIGN.md §1). */
bool wifi_sta_got_ip(void);

/* Fills `out` (dotted-quad, esp_ip4addr_ntoa()) iff wifi_sta_got_ip(). `cap`
 * must be at least 16. Returns false (out untouched) otherwise. */
bool wifi_sta_get_ip(char *out, size_t cap);

/* esp_wifi_sta_get_ap_info()'s own .rssi iff wifi_sta_associated(); false
 * (out untouched) otherwise -- same "meaningless unless associated"
 * contract wifi_policy_in_t.rssi_dbm already documents (wifi_policy.h),
 * even though this phase-1 task never calls into wifi_policy. Power effect:
 * none -- reads the driver's own cached AP record, no probe/round trip. */
bool wifi_sta_get_rssi(int *dbm);

/* UNVERIFIED / not available: ESP-IDF 5.2.1's public esp_wifi API does not
 * expose the associated AP's DTIM period anywhere this file could find
 * (wifi_ap_record_t has no such field, and no other public esp_wifi_*
 * getter returns one -- checked against
 * ~/src/esp/esp-idf/components/esp_wifi/include/esp_wifi_types.h and
 * esp_wifi.h on this machine). docs/WIFI_DESIGN.md §9's "read it from
 * wifi_ap_record_t after association" does not hold on this IDF version --
 * flagged for the architect. Always returns -1 ("unknown"); `wifi status`
 * (main.c) prints that literally rather than a fabricated number. */
int wifi_sta_get_dtim_period(void);

/* docs/WIFI_DESIGN.md §8: WIFI_PS_MIN_MODEM while "active" (the default set
 * by wifi_sta_start(), and restored by this call) -- wakes every DTIM
 * interval, the more responsive of the two modes. Call after a net_sleep()
 * WiFi-branch window ends. Power effect: same class as
 * esp_wifi_set_ps() itself -- a driver-internal power-save mode change, no
 * RRC-equivalent cost of its own. */
void wifi_sta_set_ps_active(void);

/* docs/WIFI_DESIGN.md §8: WIFI_PS_MAX_MODEM (beacon interval governed by the
 * listen_interval=10 baked into wifi_sta_associate()'s STA config) -- called
 * from net_sleep()'s WiFi branch (net.cpp) immediately before its
 * vTaskDelay(ms), the one power lever phase 1 has without CONFIG_PM_ENABLE
 * (W9): still no light sleep, so the CPU/USB stay on regardless, but the
 * radio wakes for beacons less often for the duration of the delay. */
void wifi_sta_set_ps_sleep(void);

/* One scan result row (console `wifi scan`, main.c) -- diagnostic only, not
 * used by any policy in this phase-1 task (docs/WIFI_TASKS.md W11 is the
 * first caller that scans for a reason other than "a person typed a
 * command"). */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode; /* wifi_auth_mode_t */
} wifi_sta_scan_result_t;

/* Blocking active scan (esp_wifi_scan_start(NULL, true)) on the calling
 * task -- same "blocks the console's own task, never modes_run()'s"
 * discipline as gnsstest/cafetch/smstest (main.c). Requires
 * wifi_sta_start() to have already run. Fills up to `cap` entries into
 * `out`, returns the count actually written (0..cap), or -1 on an outright
 * scan-start failure. Never returns a PSK (scan results have none to
 * return). Power effect: docs/WIFI_DESIGN.md §3b's "radio-on current during
 * a dwell" figure, ~11 channels x the driver's default per-channel dwell --
 * a few hundred ms at ~80-100 mA, once, for this one manual command. */
int wifi_sta_scan(wifi_sta_scan_result_t *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STA_H */

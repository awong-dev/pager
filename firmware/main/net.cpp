// net.cpp — network core: modem init, LTE-M attach, eDRX/PSM, TLS provisioning,
// MQTT connect/subscribe/publish/receive, and net.c's own light-sleep/RTS handling.
//
// C++ because dptechnics/walter-modem v1.5.0 is a C++ class (WalterModem, static
// methods, no extern "C"). See net.h for the C-linkage facade everything else uses.
//
// Authority: docs/PROTOCOL.md §6 (session/keepalive/eDRX), §8 (wake sources).
// Library: walter-modem v1.5.0 — see the landmines (L1-L9) called out inline;
// each was verified against the real source under managed_components/.
//
// All power-effect comments in this file are PENDING_HW: no device is attached
// to this build session. Nothing here is a measured number.

#include "net.h"
#include "pins.h"

#include "WalterModem.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";

// ---------------------------------------------------------------------------
// Configuration. Broker/credential values are provisioning placeholders —
// see docs/PROTOCOL.md §12 item 5 (NEEDS HUMAN DECISION: no per-device
// credential provisioning tooling exists yet). Do not flash these as-is.
// ---------------------------------------------------------------------------

// The UART the modem is wired to; must match the argument passed to
// WalterModem::begin() below and is used again, identically, by net_sleep().
static constexpr uart_port_t PAGER_MODEM_UART = UART_NUM_1;

static constexpr int PAGER_PDP_CTX_ID = 1;
static constexpr const char *PAGER_APN = nullptr; // carrier default; set per-SIM if required

// eDRX 20.48s / PTW 2.56s, raw 3GPP WB-S1 nibble strings (PROTOCOL.md §6.3).
// These are NOT seconds — configEDRX() splices them verbatim into AT+SQNEDRX.
static constexpr const char *PAGER_EDRX_VALUE = "0010";
static constexpr const char *PAGER_EDRX_PTW = "0001";

// Cert slot >=11, TLS profile >=2 (profile 1 is BlueCherry's), per the
// vendor's examples/mqtts and PROTOCOL.md §6.1.
static constexpr uint8_t PAGER_TLS_CA_SLOT = 12;
static constexpr int PAGER_TLS_PROFILE_ID = 2;

// PLACEHOLDERS — replace at flash time. NEEDS HUMAN DECISION (PROTOCOL.md §12 item 5).
static constexpr const char *PAGER_MQTT_BROKER_HOST = "CHANGE_ME.broker.example";
static constexpr uint16_t PAGER_MQTT_BROKER_PORT = 8883;
static constexpr const char *PAGER_DEVICE_ID = "pgr-0001";
static constexpr const char *PAGER_MQTT_USERNAME = "pgr-0001";
static constexpr const char *PAGER_MQTT_PASSWORD = "CHANGE_ME";
static constexpr uint16_t PAGER_MQTT_KEEPALIVE_S = 1800; // PROTOCOL.md §6.2

static constexpr int PAGER_ATTACH_POLL_CAP_S = 300; // F1: single-attempt cap

// PROTOCOL.md §3.3: hard envelope limit, both directions.
static constexpr uint16_t PAGER_MAX_PAYLOAD = 640;

// Placeholder CA (DigiCert Global Root CA, as used by the vendor's own
// examples/mqtts). Replace with the real broker's CA before flashing a
// device — see the NEEDS HUMAN DECISION note above.
static const char PAGER_CA_CERT_PEM[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB
CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97
nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt
43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P
T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4
gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO
BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR
TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw
DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr
hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg
06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF
PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls
YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk
CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=
-----END CERTIFICATE-----
)EOF";

// ---------------------------------------------------------------------------
// State. All of this is plain (non-RTC) static storage: it survives our
// light-sleep cycles just fine (light sleep retains RAM) and only resets on
// a real reboot, exactly like the RTC struct modes.c owns. It is NOT
// mutex-protected: the only writer besides net_init()/net_sleep() (which run
// on the caller's task, single-threaded with respect to modes.c) is the MQTT
// event handler running on _eventProcessingTask. All shared fields here are
// single-word (bool/int/uint32_t) so plain reads/writes are not torn on the
// Xtensa/RISC-V targets this project builds for; a real mutex would be
// needed if that ever stops being true (e.g. the topic buffer below is not
// concurrently written, only set once at net_init()).
// ---------------------------------------------------------------------------

static void (*s_msg_cb)(const char *, const char *, uint16_t) = nullptr;

static volatile bool s_mqtt_connected = false;
static volatile bool s_disconnect_edge = false;
static volatile int s_last_rc = 0;
static volatile net_mqtt_rc_class_t s_last_class = NET_MQTT_RC_NONE;

static volatile uint32_t s_memfull_count = 0;
static volatile uint32_t s_oversize_count = 0;

static char s_down_topic[48];
static char s_granted_edrx[16] = { 0 };

// PROTOCOL.md §3.3 cap; sized once, no malloc on the RX path (L9).
static uint8_t s_mqtt_rx_buf[PAGER_MAX_PAYLOAD];

static int64_t s_clock_epoch = 0;    // 0 = no network time yet (§3.5)
static int64_t s_clock_epoch_us = 0; // esp_timer_get_time() at the moment s_clock_epoch was read

static bool s_wake_sources_armed = false;

// ---------------------------------------------------------------------------
// F3 classification helper.
// ---------------------------------------------------------------------------

static net_mqtt_rc_class_t classify_mqtt_rc(int rc)
{
    switch (rc) {
    case WALTER_MODEM_MQTT_SUCCESS:
        return NET_MQTT_RC_OK;
    case WALTER_MODEM_MQTT_TLS:
        return NET_MQTT_RC_TLS_FAIL;
    case WALTER_MODEM_MQTT_CONN_REFUSED:
    case WALTER_MODEM_MQTT_AUTH:
    case WALTER_MODEM_MQTT_ACL_DENIED:
        return NET_MQTT_RC_PERMANENT;
    case WALTER_MODEM_MQTT_CONN_LOST:
    case WALTER_MODEM_MQTT_NO_CONN:
        return NET_MQTT_RC_TRANSIENT;
    default:
        // Unknown code: be conservative and treat as transient/retryable
        // rather than silently going permanent on something unclassified.
        return NET_MQTT_RC_TRANSIENT;
    }
}

// ---------------------------------------------------------------------------
// Event handlers. Both run on WalterModem's _eventProcessingTask
// (WalterModem.cpp:1595-1626), NOT the RX task/ISR. Calling modem APIs from
// here is the vendor's own pattern (examples/mqtts). Keep these short (L4).
// ---------------------------------------------------------------------------

static void pager_mqtt_event_handler(WMMQTTEventType event, const WMMQTTEventData *data, void *args)
{
    (void) args;

    switch (event) {
    case WALTER_MODEM_MQTT_EVENT_CONNECTED:
        if (data->rc != WALTER_MODEM_MQTT_SUCCESS) {
            s_last_rc = data->rc;
            s_last_class = classify_mqtt_rc(data->rc);
            s_disconnect_edge = true;
            ESP_LOGI(TAG, "MQTT connect failed, rc=%d", data->rc);
            break;
        }
        ESP_LOGI(TAG, "MQTT connected, resubscribing to %s", s_down_topic);
        // L3: mqttConnect() frees the ENTIRE local topic table before
        // connecting and nothing auto-resubscribes. Must resubscribe on
        // every connect, from here, exactly like the vendor's examples/mqtts.
        if (!WalterModem::mqttSubscribe(s_down_topic, 1)) {
            ESP_LOGI(TAG, "mqttSubscribe() call could not be queued");
        }
        break;

    case WALTER_MODEM_MQTT_EVENT_SUBSCRIBED:
        if (data->rc != WALTER_MODEM_MQTT_SUCCESS) {
            ESP_LOGI(TAG, "MQTT subscribe failed, rc=%d", data->rc);
            break;
        }
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT session usable (subscribed to '%s')", data->topic);
        // §5.4a: the /status online publish happens from modes.c, edge-
        // triggered on s_mqtt_connected flipping true — deliberately NOT
        // from the CONNECTED handler, so the relay only re-publishes once
        // the device can actually receive (F3).
        break;

    case WALTER_MODEM_MQTT_EVENT_PUBLISHED:
        if (data->rc != WALTER_MODEM_MQTT_SUCCESS) {
            ESP_LOGD(TAG, "PUBACK missing for mid=%d rc=%d", data->mid, data->rc);
        } else {
            ESP_LOGD(TAG, "PUBACK mid=%d", data->mid);
        }
        // §4.1 r6 / §4.2: freeing the matching pending_ack/pending_up RTC
        // entry belongs to msg.c (Phase 5), which is the thing that knows
        // which entry a given publish was for. No such entries exist yet.
        break;

    case WALTER_MODEM_MQTT_EVENT_MESSAGE:
        if (data->msg_length > PAGER_MAX_PAYLOAD) {
            // F6: oversize payload. Log, count, do not ack/render, but still
            // drain it with a scratch read so it doesn't wedge the modem's
            // buffer into F5.
            s_oversize_count = s_oversize_count + 1; // volatile: avoid deprecated ++ (C++20)
            ESP_LOGI(TAG, "oversize MQTT message dropped: %u bytes > %u cap",
                     (unsigned) data->msg_length, (unsigned) PAGER_MAX_PAYLOAD);
            WalterModem::mqttReceive(data->topic, data->mid, s_mqtt_rx_buf, sizeof(s_mqtt_rx_buf));
            break;
        }

        // L1/L2: never mqttDidRing(). Fetch by the real mid from this event.
        if (!WalterModem::mqttReceive(data->topic, data->mid, s_mqtt_rx_buf, data->msg_length)) {
            ESP_LOGI(TAG, "mqttReceive() failed for mid=%d", data->mid);
            break;
        }

        if (s_msg_cb) {
            s_msg_cb(data->topic, (const char *) s_mqtt_rx_buf, data->msg_length);
        }
        break;

    case WALTER_MODEM_MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        s_last_rc = data->rc;
        s_last_class = classify_mqtt_rc(data->rc);
        s_disconnect_edge = true;
        ESP_LOGI(TAG, "MQTT disconnected, rc=%d", data->rc);
        break;

    case WALTER_MODEM_MQTT_EVENT_MEMORY_FULL:
        s_memfull_count = s_memfull_count + 1; // volatile: avoid deprecated ++ (C++20)
        ESP_LOGI(TAG, "MQTT modem buffer MEMORY_FULL (count this cycle=%u)",
                 (unsigned) s_memfull_count);
        break;
    }
}

static void pager_network_event_handler(WMNetworkEventType event, const WMNetworkEventData *data, void *args)
{
    (void) args;

    if (event == WALTER_MODEM_NETWORK_EVENT_REG_STATE_CHANGE) {
        ESP_LOGI(TAG, "network registration state -> %d", (int) data->cereg.state);
        return;
    }

    if (event == WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED) {
        strncpy(s_granted_edrx, data->edrx.nwProvidedEdrx, sizeof(s_granted_edrx) - 1);
        s_granted_edrx[sizeof(s_granted_edrx) - 1] = '\0';
        ESP_LOGI(TAG, "granted eDRX=%s PTW=%s (requested %s/%s)", data->edrx.nwProvidedEdrx,
                 data->edrx.pagingTimeWindow, PAGER_EDRX_VALUE, PAGER_EDRX_PTW);
        // M4 / §6.5: a granted cycle above 20.48s breaks the 30s sleep-mode
        // latency budget outright. Log loudly rather than assert-crash —
        // this needs a human to see it and retune T, not a reboot loop.
        if (strcmp(data->edrx.nwProvidedEdrx, PAGER_EDRX_VALUE) != 0) {
            ESP_LOGI(TAG, "WARNING: granted eDRX != requested 20.48s - §6.5 latency "
                          "budget assumption violated, see PROTOCOL.md M4");
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" bool net_init(void)
{
    // Power effect: WalterModem::begin() only calls reset() when
    // esp_sleep_get_wakeup_cause() == UNDEFINED (L5) - i.e. on a real
    // reboot, not a light-sleep continuation. This design never deep
    // sleeps, so that covers every meaningful invocation of net_init().
    if (!WalterModem::begin(PAGER_MODEM_UART)) {
        ESP_LOGI(TAG, "WalterModem::begin() failed");
        return false;
    }

    WalterModem::setMQTTEventHandler(pager_mqtt_event_handler, nullptr);
    WalterModem::setNetworkEventHandler(pager_network_event_handler, nullptr);

    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGI(TAG, "setOpState(NO_RF) failed");
        return false;
    }

    if (!WalterModem::definePDPContext(PAGER_PDP_CTX_ID, PAGER_APN)) {
        ESP_LOGI(TAG, "definePDPContext() failed");
        return false;
    }

    // Power effect: this is the setting that makes sleep-mode paging cheap;
    // see PROTOCOL.md §8.4 for the (PENDING_HW) current budget it buys.
    if (!WalterModem::configEDRX(WALTER_MODEM_EDRX_ENABLE_WITH_RESULT, PAGER_EDRX_VALUE,
                                 PAGER_EDRX_PTW)) {
        ESP_LOGI(TAG, "configEDRX() failed - continuing without a granted eDRX confirmation");
    }

    // PSM explicitly disabled (PROTOCOL.md §6.3): PSM would suspend paging
    // entirely, which breaks the <=30s sleep-mode delivery target.
    if (!WalterModem::configPSM(WALTER_MODEM_PSM_DISABLE)) {
        ESP_LOGI(TAG, "configPSM(DISABLE) failed");
    }

    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        ESP_LOGI(TAG, "setOpState(FULL) failed");
        return false;
    }

    if (!WalterModem::setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        ESP_LOGI(TAG, "setNetworkSelectionMode() failed");
        return false;
    }

    // F1: single-attempt wait, capped at 300s. modes.c is responsible for
    // the 5/15/60/300s backoff across repeated net_init() calls; this loop
    // is not itself a retry loop, so it never busy-spins setOpState(FULL).
    bool attached = false;
    for (int waited_s = 0; waited_s < PAGER_ATTACH_POLL_CAP_S; waited_s++) {
        WalterModemNetworkRegState st = WalterModem::getNetworkRegState();
        if (st == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
            st == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) {
            attached = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!attached) {
        ESP_LOGI(TAG, "network attach timed out after %ds (F1)", PAGER_ATTACH_POLL_CAP_S);
        return false;
    }
    ESP_LOGI(TAG, "network attached");

    // §3.5: seed ts from the network clock (NITZ via getClock()). On
    // failure the device publishes ts:0 forever - no SNTP path is added.
    WalterModemRsp rsp = {};
    if (WalterModem::getClock(&rsp)) {
        s_clock_epoch = rsp.data.clock.epochTime;
        s_clock_epoch_us = esp_timer_get_time();
        ESP_LOGI(TAG, "clock seeded from network: epoch=%lld", (long long) s_clock_epoch);
    } else {
        s_clock_epoch = 0;
        ESP_LOGI(TAG, "no network clock available; ts will read 0 (PROTOCOL.md §3.5)");
    }

    // TLS provisioning (PROTOCOL.md §6.1): cert slot >=11, TLS profile >=2.
    // PAGER_CA_CERT_PEM above is a placeholder - see the NEEDS HUMAN
    // DECISION note at the top of this file before flashing a real device.
    if (!WalterModem::tlsWriteCredential(false, PAGER_TLS_CA_SLOT, PAGER_CA_CERT_PEM)) {
        ESP_LOGI(TAG, "tlsWriteCredential() failed");
        return false;
    }
    if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_CA,
                                       WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT)) {
        ESP_LOGI(TAG, "tlsConfigProfile() failed");
        return false;
    }

    snprintf(s_down_topic, sizeof(s_down_topic), "pager/%s/down", PAGER_DEVICE_ID);

    if (!WalterModem::mqttConfig(PAGER_DEVICE_ID, PAGER_MQTT_USERNAME, PAGER_MQTT_PASSWORD,
                                 PAGER_TLS_PROFILE_ID)) {
        ESP_LOGI(TAG, "mqttConfig() failed");
        return false;
    }

    return true;
}

extern "C" bool net_session_up(void)
{
    // Power effect: one TLS handshake, ~5kB (PROTOCOL.md §7.2/§7.3), plus
    // the RRC time it takes. Never call this on a timer - only after
    // net_init() and after a detected session loss (F3).
    s_disconnect_edge = false;
    if (!WalterModem::mqttConnect(PAGER_MQTT_BROKER_HOST, PAGER_MQTT_BROKER_PORT,
                                  PAGER_MQTT_KEEPALIVE_S)) {
        ESP_LOGI(TAG, "mqttConnect() call could not be queued");
        return false;
    }
    ESP_LOGI(TAG, "MQTT connect issued to %s:%u", PAGER_MQTT_BROKER_HOST, PAGER_MQTT_BROKER_PORT);
    return true;
}

extern "C" void net_session_down(void)
{
    // F3 recovery path only (never called on a timer or speculatively).
    WalterModem::mqttDisconnect();
    s_mqtt_connected = false;
}

extern "C" bool net_publish(const char *topic, char *buf, uint16_t len, uint8_t qos)
{
    if (len > PAGER_MAX_PAYLOAD) {
        ESP_LOGI(TAG, "refusing to publish %u bytes > %u cap (PROTOCOL.md §3.3)",
                 (unsigned) len, (unsigned) PAGER_MAX_PAYLOAD);
        return false;
    }
    // L6: mqttPublish() takes non-const uint8_t*; publish from a mutable buffer.
    return WalterModem::mqttPublish(topic, (uint8_t *) buf, len, qos);
}

extern "C" void net_set_msg_cb(void (*cb)(const char *topic, const char *body, uint16_t len))
{
    s_msg_cb = cb;
}

extern "C" void net_sleep(uint32_t ms)
{
    // Power effect: ESP32 draws the vendor-documented ~1 mA light-sleep
    // floor (WalterModem.h sleep() doc comment) for up to `ms`; the modem
    // is untouched and keeps paging on its own eDRX cycle.
    //
    // This is deliberately NOT WalterModem::sleep(ms, true): that call is
    // timer-only (WalterModem.cpp:4413-4450) and would starve out the
    // button's ext0 wake if we let it own esp_light_sleep_start(). We
    // replicate its RTS choreography by hand instead.
    if (!s_wake_sources_armed) {
        esp_sleep_enable_ext0_wakeup((gpio_num_t) PAGER_PIN_BUTTON, 0 /* active low */);
        s_wake_sources_armed = true;
    }
    esp_sleep_enable_timer_wakeup((uint64_t) ms * 1000ULL);

    // Disable RTS (drive it high) so the modem is free to sleep, exactly as
    // WalterModem::sleep(_, true) does for its own light-sleep path.
    // §8.3/M5: whether the Sequans queues or drops URCs while CTS is
    // deasserted on its side is UNVERIFIED - the riskiest assumption in
    // this whole design.
    uart_set_hw_flow_ctrl(PAGER_MODEM_UART, UART_HW_FLOWCTRL_DISABLE, 0);
    gpio_set_direction((gpio_num_t) CONFIG_WALTER_MODEM_PIN_RTS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t) CONFIG_WALTER_MODEM_PIN_RTS, 1);

    esp_light_sleep_start();

    // Re-enable RTS/CTS hardware flow control after waking, exactly as the
    // library does on its own light-sleep return path.
    //
    // NOTE: WALTER_MODEM_PIN_RTS/CTS/TX/RX and UART_BUF_THRESHOLD (as named
    // in WalterModem.cpp:4413-4450) are declared `static constexpr` *inside
    // WalterModem.cpp itself* (WalterModem.cpp:82-133), i.e. with internal
    // linkage private to that translation unit - they are NOT visible from
    // net.cpp despite appearing unqualified in the header's doc comments.
    // We use the equivalent CONFIG_WALTER_MODEM_PIN_* / CONFIG_UART_BUF_
    // THRESHOLD Kconfig macros instead (from the generated sdkconfig.h,
    // pulled in transitively via WalterModem.h): same values (RX14/TX48/
    // RTS21/CTS47, threshold 122 by default), Kconfig-overridable, and
    // actually reachable from this file.
    uart_set_pin(PAGER_MODEM_UART, CONFIG_WALTER_MODEM_PIN_TX, CONFIG_WALTER_MODEM_PIN_RX,
                 CONFIG_WALTER_MODEM_PIN_RTS, CONFIG_WALTER_MODEM_PIN_CTS);
    uart_set_hw_flow_ctrl(PAGER_MODEM_UART, UART_HW_FLOWCTRL_CTS_RTS, CONFIG_UART_BUF_THRESHOLD);
}

extern "C" bool net_check(void)
{
    // Power effect: one AT round trip ("AT" / "OK"), no RRC of its own.
    if (!WalterModem::checkComm()) {
        return false;
    }
    WalterModemNetworkRegState st = WalterModem::getNetworkRegState();
    return (st == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
            st == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
}

extern "C" bool net_recover_modem(void)
{
    // F4 ONLY. This is the one legal caller of reset() outside begin()'s
    // own cold-boot path (L5). Power effect: full modem power cycle +
    // re-attach - modes.c must rate-limit this to 1/10min.
    ESP_LOGI(TAG, "modem unresponsive: issuing hard reset + full re-init (F4)");
    s_mqtt_connected = false;
    if (!WalterModem::reset()) {
        ESP_LOGI(TAG, "WalterModem::reset() failed");
        return false;
    }
    // WalterModem::begin() is documented to no-op on the 2nd+ call, so this
    // safely redoes opstate/PDP/eDRX/PSM/attach/clock/TLS/mqttConfig only.
    return net_init();
}

extern "C" bool net_get_clock(int64_t *epoch_s)
{
    if (s_clock_epoch == 0) {
        if (epoch_s) {
            *epoch_s = 0;
        }
        return false;
    }
    int64_t elapsed_s = (esp_timer_get_time() - s_clock_epoch_us) / 1000000;
    if (epoch_s) {
        *epoch_s = s_clock_epoch + elapsed_s;
    }
    return true;
}

extern "C" void net_get_mqtt_status(net_mqtt_status_t *out)
{
    if (!out) {
        return;
    }
    out->mqtt_connected = s_mqtt_connected;
    out->disconnect_edge = s_disconnect_edge;
    out->last_rc = s_last_rc;
    out->last_class = s_last_class;
}

extern "C" void net_ack_disconnect_edge(void)
{
    s_disconnect_edge = false;
}

extern "C" uint32_t net_take_memfull_delta(void)
{
    uint32_t v = s_memfull_count;
    s_memfull_count = 0;
    return v;
}

extern "C" uint32_t net_take_oversize_delta(void)
{
    uint32_t v = s_oversize_count;
    s_oversize_count = 0;
    return v;
}

extern "C" void net_get_granted_edrx(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    strncpy(out, s_granted_edrx, out_size - 1);
    out[out_size - 1] = '\0';
}

extern "C" const char *net_get_device_id(void)
{
    return PAGER_DEVICE_ID;
}

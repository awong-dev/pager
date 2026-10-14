// Keepalive probe: does the GM02SP send MQTT PINGREQs on its own while idle under eDRX
// with the ESP32 light-sleeping like the pager? The broker-side capture is the judge;
// this log only shows when the session died relative to the keepalive.
// Sequence mirrors firmware/main/net.cpp (net_bringup, net_sleep). See README.md.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "WalterModem.h"
#include "pins.h"           // firmware/main/pins.h: the only source of GPIO numbers
#include "placeholder_ca.h" // PAGER_PLACEHOLDER_CA_PEM (a TLS profile must name a non-empty CA slot)

static const char *TAG = "kaprobe";

// Kconfig bools are undefined when off, so map them to constants for use in plain C++ expressions.
#ifdef CONFIG_PROBE_RECONNECT
static constexpr bool kReconnect = true;
#else
static constexpr bool kReconnect = false;
#endif
#ifdef CONFIG_PROBE_AT_TRACE
static constexpr bool kAtTrace = true;
#else
static constexpr bool kAtTrace = false;
#endif
#ifdef CONFIG_PROBE_EDRX
static constexpr bool kEdrx = true;
#else
static constexpr bool kEdrx = false;
#endif
#ifdef CONFIG_PROBE_LIGHT_SLEEP
static constexpr bool kLightSleep = true;
#else
static constexpr bool kLightSleep = false;
#endif
#ifdef CONFIG_PROBE_WAKE_PROBE
static constexpr bool kWakeProbe = true;
#else
static constexpr bool kWakeProbe = false;
#endif

static constexpr uart_port_t MODEM_UART = UART_NUM_1; // net.cpp PAGER_MODEM_UART
static constexpr int PDP_CTX_ID = 1;                  // net.cpp PAGER_PDP_CTX_ID
static constexpr int64_t HEARTBEAT_US = 60LL * 1000000;
static constexpr EventBits_t EV_CONNECT_RESULT = BIT0, EV_SUBSCRIBED = BIT1;

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_at_mtx;    // held by the event task during AT transactions and by app_main across sleep entry
static SemaphoreHandle_t s_probe_sem; // given by probe_cb
static volatile bool s_probe_out;
static volatile int s_state;          // 0 idle, 1 connecting, 2 connected, 3 subscribed, 4 disconnected
static volatile bool s_disc_edge;
static volatile int s_last_rc;
static uint32_t s_msgs, s_wakes;
static int64_t s_epoch_base;          // modem UTC at s_epoch_base_up_us, 0 = unknown
static int64_t s_epoch_base_up_us;
static char s_down_topic[96];
static uint8_t s_rx[256];

static const char *const STATE_NAME[] = {"idle", "connecting", "connected", "subscribed", "DISCONNECTED"};

// "T+<uptime s> <UTC>" prefix; esp_timer keeps counting through light sleep.
static const char *stamp(void)
{
    static char b[64];
    int64_t up = esp_timer_get_time();
    int n = snprintf(b, sizeof b, "T+%lld.%03lld", up / 1000000, (up / 1000) % 1000);
    if (s_epoch_base) {
        time_t t = (time_t) (s_epoch_base + (up - s_epoch_base_up_us) / 1000000);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(b + n, sizeof b - n, " %Y-%m-%dT%H:%M:%SZ", &tm);
    }
    return b;
}
#define PLOG(fmt, ...) ESP_LOGI(TAG, "[%s] " fmt, stamp(), ##__VA_ARGS__)

static void network_handler(WMNetworkEventType ev, const WMNetworkEventData *d, void *)
{
    if (ev == WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED) {
        PLOG("granted eDRX=%s PTW=%s (requested %s)", d->edrx.nwProvidedEdrx,
             d->edrx.pagingTimeWindow, d->edrx.requestedEdrx);
    } else if (ev == WALTER_MODEM_NETWORK_EVENT_REG_STATE_CHANGE) {
        ESP_LOGD(TAG, "CEREG state=%d", (int) d->cereg.state);
    }
}

static void mqtt_handler(WMMQTTEventType ev, const WMMQTTEventData *d, void *)
{
    switch (ev) {
    case WALTER_MODEM_MQTT_EVENT_CONNECTED:
        PLOG("MQTT CONNECTED rc=%d", d->rc);
        if (d->rc != WALTER_MODEM_MQTT_SUCCESS) {
            s_last_rc = d->rc; s_state = 4; s_disc_edge = true;
            xEventGroupSetBits(s_ev, EV_CONNECT_RESULT);
            break;
        }
        s_state = 2;
        xEventGroupSetBits(s_ev, EV_CONNECT_RESULT);
        // mqttConnect() frees the vendor topic table, so subscribe on every connect (net.cpp does too).
        xSemaphoreTake(s_at_mtx, portMAX_DELAY); // keeps app_main from light-sleeping mid-transaction
        if (!WalterModem::mqttSubscribe(s_down_topic, 1)) {
            PLOG("mqttSubscribe could not be queued");
        }
        xSemaphoreGive(s_at_mtx);
        break;
    case WALTER_MODEM_MQTT_EVENT_SUBSCRIBED:
        PLOG("MQTT SUBSCRIBED topic=%s rc=%d", d->topic, d->rc);
        s_state = 3;
        xEventGroupSetBits(s_ev, EV_SUBSCRIBED);
        break;
    case WALTER_MODEM_MQTT_EVENT_DISCONNECTED:
        PLOG("MQTT +SQNSMQTTONDISCONNECT reason rc=%d", d->rc);
        s_last_rc = d->rc; s_state = 4; s_disc_edge = true;
        break;
    case WALTER_MODEM_MQTT_EVENT_MESSAGE: {
        s_msgs++;
        PLOG("MQTT MESSAGE topic=%s len=%u qos=%u mid=%d", d->topic, d->msg_length, d->qos, d->mid);
        uint16_t n = d->msg_length < sizeof s_rx - 1 ? d->msg_length : sizeof s_rx - 1;
        xSemaphoreTake(s_at_mtx, portMAX_DELAY);
        if (WalterModem::mqttReceive(d->topic, d->mid, s_rx, n, NULL, NULL, NULL, 1, pdMS_TO_TICKS(15000))) {
            s_rx[n] = 0;
            PLOG("MQTT payload: %s", (const char *) s_rx);
        } else {
            PLOG("mqttReceive failed mid=%d", d->mid);
        }
        xSemaphoreGive(s_at_mtx);
        break;
    }
    case WALTER_MODEM_MQTT_EVENT_PUBLISHED:
        PLOG("MQTT PUBLISHED mid=%d (unexpected: probe never publishes)", d->mid);
        break;
    case WALTER_MODEM_MQTT_EVENT_MEMORY_FULL:
        PLOG("MQTT MEMORY_FULL");
        break;
    }
}

// Bring-up as net_bringup(); returns false on a hard failure.
static bool modem_bringup(void)
{
    PLOG("modem bring-up starting");
    if (!WalterModem::begin(MODEM_UART)) { PLOG("WalterModem::begin failed"); return false; }
    WalterModem::setMQTTEventHandler(mqtt_handler, nullptr);
    WalterModem::setNetworkEventHandler(network_handler, nullptr);
    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_NO_RF)) { PLOG("setOpState(NO_RF) failed"); return false; }
    const char *apn = CONFIG_PROBE_APN[0] ? CONFIG_PROBE_APN : nullptr;
    if (!WalterModem::definePDPContext(PDP_CTX_ID, apn)) { PLOG("definePDPContext failed"); return false; }
    // As net.cpp apply_pdp_auth(): the library's setPDPAuthParams() never emits, so raw AT+CGAUTH
    // (PAP). The modem keeps it across resets, so clear it when no user is configured.
    {
        char cmd[96];
        if (CONFIG_PROBE_APN_USER[0])
            snprintf(cmd, sizeof(cmd), "AT+CGAUTH=%d,1,\"%s\",\"%s\"", PDP_CTX_ID, CONFIG_PROBE_APN_USER,
                     CONFIG_PROBE_APN_PASS);
        else
            snprintf(cmd, sizeof(cmd), "AT+CGAUTH=%d,0", PDP_CTX_ID);
        if (!WalterModem::sendCmd(cmd)) PLOG("%s rejected - continuing", cmd);
        else PLOG("pdp: apn=%s auth=%s", apn ? apn : "(blank)", CONFIG_PROBE_APN_USER[0] ? "PAP" : "none");
    }

#if CONFIG_PROBE_EDRX
    // Power effect: enables paging on the eDRX cycle instead of continuous DRX.
    if (!WalterModem::configEDRX(WALTER_MODEM_EDRX_ENABLE_WITH_RESULT, CONFIG_PROBE_EDRX_VALUE, CONFIG_PROBE_EDRX_PTW))
        PLOG("configEDRX failed - continuing");
#else
    if (!WalterModem::configEDRX(WALTER_MODEM_EDRX_DISABLE)) PLOG("configEDRX(DISABLE) failed - continuing");
#endif
    // Same host-interface power saving the pager pins (net.cpp). Power effect: none beyond one AT.
    if (!WalterModem::sendCmd("AT+SQNIPSCFG=1,100")) PLOG("AT+SQNIPSCFG=1,100 not accepted - continuing");
    // Power effect: PSM would stop paging entirely.
    if (!WalterModem::configPSM(WALTER_MODEM_PSM_DISABLE)) PLOG("configPSM(DISABLE) failed");
    // Power effect: radio on, attach starts.
    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_FULL)) { PLOG("setOpState(FULL) failed"); return false; }
    if (!WalterModem::setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        PLOG("setNetworkSelectionMode failed");
        return false;
    }
    for (int s = 0; s < CONFIG_PROBE_ATTACH_TIMEOUT_S; s++) {
        auto st = WalterModem::getNetworkRegState();
        if (st == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME || st == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) {
            PLOG("attached after ~%d s (cereg=%d)", s, (int) st);
            return true;
        }
        if (s % 5 == 0) PLOG("attaching... %d s (cereg=%d)", s, (int) st);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    PLOG("no LTE attach within %d s", CONFIG_PROBE_ATTACH_TIMEOUT_S);
    return false;
}

static void seed_clock(void)
{
    for (int i = 0; i < 10 && !s_epoch_base; i++) {
        WalterModemRsp rsp = {};
        if (WalterModem::getClock(&rsp) && rsp.data.clock.epochTime >= 1704067200LL &&
            rsp.data.clock.epochTime < 3124224000LL) { // same sanity window as net.cpp (rejects the 2070 artefact)
            s_epoch_base = rsp.data.clock.epochTime;
            s_epoch_base_up_us = esp_timer_get_time();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// TLS profile (mandatory, else the modem sends plaintext), MQTT config, connect. Power effect: one TLS handshake.
static bool mqtt_start(void)
{
    // The modem needs a real certificate in the slot the profile names, even with VALIDATION_NONE. Use the
    // pager's slot 12 and, only if it is empty, write the pager's own placeholder CA there, exactly as
    // net.cpp ensure_ca_slot_populated() does -- so a CA the pager pinned there is never overwritten.
    {
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "AT+SQNSNVR=\"certificate\",%d", CONFIG_PROBE_CA_SLOT);
        if (WalterModem::sendCmd(cmd, "+SQNSNVR: ")) {
            PLOG("CA slot %d already holds a certificate - left as is", CONFIG_PROBE_CA_SLOT);
        } else if (WalterModem::tlsWriteCredential(false, CONFIG_PROBE_CA_SLOT, PAGER_PLACEHOLDER_CA_PEM)) {
            PLOG("CA slot %d was empty - placeholder CA written", CONFIG_PROBE_CA_SLOT);
        } else {
            PLOG("CA slot %d empty and the placeholder write failed", CONFIG_PROBE_CA_SLOT);
        }
    }
    if (!WalterModem::tlsConfigProfile(CONFIG_PROBE_TLS_PROFILE, WALTER_MODEM_TLS_VALIDATION_NONE,
                                       WALTER_MODEM_TLS_VERSION_12, CONFIG_PROBE_CA_SLOT)) {
        PLOG("tlsConfigProfile failed");
        return false;
    }
    // 12 Oct 2026 bench: AT+SQNSMQTTCFG answered +CME ERROR: 4 ~1 s after attach. Log the engine's state, then
    // retry with a pause in case the MQTT stack is not ready that soon after registration.
    WalterModem::sendCmd("AT+SQNSMQTTCFG?");
    WalterModem::sendCmd("AT+CGPADDR=1");
    bool cfg_ok = false;
    for (int i = 0; i < 6 && !cfg_ok; i++) {
        if (i) {
            PLOG("mqttConfig failed (attempt %d), retrying in 5 s", i);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        cfg_ok = WalterModem::mqttConfig(CONFIG_PROBE_CLIENT_ID, CONFIG_PROBE_CLIENT_ID, CONFIG_PROBE_CLIENT_ID,
                                         CONFIG_PROBE_TLS_PROFILE); // as net.cpp: id as user/pass
    }
    if (!cfg_ok) {
        // Diagnose: is the CA slot empty, what does profile 2 hold, and does CFG work with no TLS profile at all?
        PLOG("mqttConfig failed - diagnostics follow");
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "AT+SQNSNVR=\"certificate\",%d", CONFIG_PROBE_CA_SLOT);
        WalterModem::sendCmd(cmd);
        WalterModem::sendCmd("AT+SQNSPCFG?");
        bool plain_ok = WalterModem::mqttConfig(CONFIG_PROBE_CLIENT_ID, CONFIG_PROBE_CLIENT_ID,
                                                CONFIG_PROBE_CLIENT_ID, 0);
        PLOG("diag: AT+SQNSMQTTCFG without a TLS profile %s", plain_ok ? "ACCEPTED" : "also rejected");
        return false;
    }
    s_disc_edge = false;
    s_state = 1;
    if (!WalterModem::mqttConnect(CONFIG_PROBE_BROKER_HOST, CONFIG_PROBE_BROKER_PORT, CONFIG_PROBE_KEEPALIVE_S)) {
        PLOG("mqttConnect could not be queued");
        return false;
    }
    PLOG("mqttConnect issued to %s:%d client=%s keepalive=%d s", CONFIG_PROBE_BROKER_HOST,
         CONFIG_PROBE_BROKER_PORT, CONFIG_PROBE_CLIENT_ID, CONFIG_PROBE_KEEPALIVE_S);
    return true;
}

#if CONFIG_PROBE_LIGHT_SLEEP
static void probe_cb(const WalterModemRsp *rsp, void *)
{
    ESP_LOGD(TAG, "probe answered result=%d", rsp ? (int) rsp->result : -1);
    s_probe_out = false;
    xSemaphoreGive(s_probe_sem);
}

// Per-wake URC drain, as net_urc_probe(): wake chars, 60 ms, one bare AT. Power effect: ~60 ms awake + one AT.
static void wake_probe(void)
{
    if (s_probe_out) return;
    xSemaphoreTake(s_probe_sem, 0);
    s_probe_out = true;
    uart_write_bytes(MODEM_UART, "\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(60));
    WalterModem::checkComm(NULL, probe_cb, NULL, 1, pdMS_TO_TICKS(15000));
}

// Light sleep for ms with the RTS choreography of net_sleep(). Power effect: ESP32 ~1 mA; modem pages on its own cycle.
static void light_sleep(uint32_t ms)
{
    xSemaphoreTake(s_at_mtx, portMAX_DELAY); // never deassert RTS mid-transaction
    esp_sleep_enable_timer_wakeup((uint64_t) ms * 1000ULL);
    uart_set_hw_flow_ctrl(MODEM_UART, UART_HW_FLOWCTRL_DISABLE, 0);
    gpio_set_direction((gpio_num_t) CONFIG_WALTER_MODEM_PIN_RTS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t) CONFIG_WALTER_MODEM_PIN_RTS, 1);
    // Without this the sleep GPIO workaround floats RTS during sleep (docs/RCA_SLEEP_URC.md).
    gpio_sleep_sel_dis((gpio_num_t) CONFIG_WALTER_MODEM_PIN_RTS);
    vTaskDelay(pdMS_TO_TICKS(20)); // let the USB-JTAG log drain; the pager does not, the probe wants its last line
    esp_light_sleep_start();
    uart_set_pin(MODEM_UART, CONFIG_WALTER_MODEM_PIN_TX, CONFIG_WALTER_MODEM_PIN_RX,
                 CONFIG_WALTER_MODEM_PIN_RTS, CONFIG_WALTER_MODEM_PIN_CTS);
    uart_set_hw_flow_ctrl(MODEM_UART, UART_HW_FLOWCTRL_CTS_RTS, CONFIG_UART_BUF_THRESHOLD);
    xSemaphoreGive(s_at_mtx);
}
#endif

extern "C" void app_main(void)
{
    s_ev = xEventGroupCreate();
    s_at_mtx = xSemaphoreCreateMutex();
    s_probe_sem = xSemaphoreCreateBinary();
    snprintf(s_down_topic, sizeof s_down_topic, "probe/%s/down", CONFIG_PROBE_CLIENT_ID);
    if (kAtTrace) esp_log_level_set("WalterModem", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "keepalive probe: broker=%s:%d client=%s keepalive=%ds edrx=%s(%s/%s) light_sleep=%s slice=%dms "
                  "wake_probe=%s reconnect=%s",
             CONFIG_PROBE_BROKER_HOST, CONFIG_PROBE_BROKER_PORT, CONFIG_PROBE_CLIENT_ID, CONFIG_PROBE_KEEPALIVE_S,
             kEdrx ? "on" : "off",
#if CONFIG_PROBE_EDRX
             CONFIG_PROBE_EDRX_VALUE, CONFIG_PROBE_EDRX_PTW,
#else
             "-", "-",
#endif
             kLightSleep ? "on" : "off",
#if CONFIG_PROBE_LIGHT_SLEEP
             CONFIG_PROBE_SLEEP_SLICE_MS,
             kWakeProbe ? "on" : "off",
#else
             0, "off",
#endif
             kReconnect ? "on" : "off");

    // Keep the pager's gated peripheral rails (CardKB, e-ink) off so they do not distort the picture.
    // Power effect: both rails unpowered, held through sleep. The modem is not on either gate.
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PAGER_PIN_3V3_EN) | (1ULL << PAGER_PIN_DISP_VCC_EN);
    cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t) PAGER_PIN_3V3_EN, 1);                         // active-low: off
    gpio_set_level((gpio_num_t) PAGER_PIN_DISP_VCC_EN, PAGER_DISP_VCC_EN_OFF);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_3V3_EN);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_VCC_EN);
    vTaskDelay(pdMS_TO_TICKS(20)); // >=15 ms settle rule before touching anything

    if (!modem_bringup()) { PLOG("bring-up failed, halting"); vTaskSuspend(NULL); }
    seed_clock();
    PLOG("modem clock %s", s_epoch_base ? "seeded" : "unavailable (uptime-only stamps)");
    if (!mqtt_start()) { PLOG("MQTT start failed, halting"); vTaskSuspend(NULL); }
    xEventGroupWaitBits(s_ev, EV_CONNECT_RESULT, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    xEventGroupWaitBits(s_ev, EV_SUBSCRIBED, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    PLOG("idle phase begins: state=%s. No publishes, no re-subscribes from here on.", STATE_NAME[s_state]);

    int64_t next_hb = esp_timer_get_time() + HEARTBEAT_US;
    bool reported_disc = false;
    for (;;) {
        if (s_disc_edge && !reported_disc) {
            reported_disc = true;
            PLOG("SESSION DEAD rc=%d (uptime above; compare to keepalive %d s)", s_last_rc, CONFIG_PROBE_KEEPALIVE_S);
            if (kReconnect) {
                PLOG("reconnecting (PROBE_RECONNECT=y)");
                reported_disc = false;
                if (!mqtt_start()) reported_disc = true;
            }
        }
        if (esp_timer_get_time() >= next_hb) {
            next_hb += HEARTBEAT_US;
            PLOG("heartbeat state=%s msgs=%u wakes=%u", STATE_NAME[s_state], (unsigned) s_msgs, (unsigned) s_wakes);
        }
#if CONFIG_PROBE_LIGHT_SLEEP
        light_sleep(CONFIG_PROBE_SLEEP_SLICE_MS);
        s_wakes++;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PROBE_POST_WAKE_YIELD_MS));
#if CONFIG_PROBE_WAKE_PROBE
        wake_probe();
        if (s_probe_out) xSemaphoreTake(s_probe_sem, pdMS_TO_TICKS(CONFIG_PROBE_WAKE_PROBE_WAIT_MS));
#endif
#else
        vTaskDelay(pdMS_TO_TICKS(1000)); // control run: awake, RTS asserted, library drains URCs itself
#endif
    }
}

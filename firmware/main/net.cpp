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
#include "ident.h"

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
// Configuration. Per-device identity (host/port/dev_id/mqtt_pw/apn/CA) comes
// from ident.c/h (docs/DEVICE_PLAN.md §3.2 step 3, §3.4), written by setup.c
// (F3.5) from the bootstrap bundle. Only fleet-wide constants live here.
// ---------------------------------------------------------------------------

// The UART the modem is wired to; must match the argument passed to
// WalterModem::begin() below and is used again, identically, by net_sleep().
static constexpr uart_port_t PAGER_MODEM_UART = UART_NUM_1;

static constexpr int PAGER_PDP_CTX_ID = 1;

// eDRX 20.48s / PTW 2.56s, raw 3GPP WB-S1 nibble strings (PROTOCOL.md §6.3).
// These are NOT seconds — configEDRX() splices them verbatim into AT+SQNEDRX.
static constexpr const char *PAGER_EDRX_VALUE = "0010";
static constexpr const char *PAGER_EDRX_PTW = "0001";

// Cert slot >=11, TLS profile >=2 (profile 1 is BlueCherry's), per the
// vendor's examples/mqtts and PROTOCOL.md §6.1.
static constexpr uint8_t PAGER_TLS_CA_SLOT = 12;
static constexpr int PAGER_TLS_PROFILE_ID = 2;

// DEVICE_PLAN.md §3.2 step 3: a second, unpinned profile for the one-time
// bootstrap MQTT hop only (setup.c, F3.5). Never shares a slot/profile with
// the production connection above.
static constexpr int PAGER_TLS_BOOTSTRAP_PROFILE_ID = 3;

static constexpr uint16_t PAGER_MQTT_KEEPALIVE_S = 1800; // PROTOCOL.md §6.2

static constexpr int PAGER_ATTACH_POLL_CAP_S = 300; // F1: single-attempt cap

// PROTOCOL.md §3.3: hard envelope limit, both directions.
static constexpr uint16_t PAGER_MAX_PAYLOAD = 640;

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

static volatile bool s_handler_busy = false; // true while the MQTT event
                                             // handler is inside an AT
                                             // transaction or the app
                                             // callback (see the RTS interlock below)

static volatile uint32_t s_memfull_count = 0;
static volatile uint32_t s_oversize_count = 0;

static char s_down_topic[48];
static char s_granted_edrx[16] = { 0 };

// DEVICE_PLAN.md §3.3: the hash of the CA last written to modem NVRAM slot
// PAGER_TLS_CA_SLOT, so repeated net_init() calls within one power session
// (F4 recovery) skip the NVRAM write when ident's ca_hash has not changed.
// Deliberately NOT in RTC/NVS: it only needs to survive net_recover_modem(),
// not a real reboot — a real reboot always rewriting once is harmless and
// simpler than persisting this across resets.
static bool s_ca_written = false;
static uint8_t s_ca_written_hash[IDENT_CA_HASH_LEN];

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
        // Same RTS interlock as the MESSAGE case below: this is an AT
        // transaction issued from the event task, so modes_run() must not
        // light-sleep (and deassert RTS) underneath it.
        s_handler_busy = true;
        if (!WalterModem::mqttSubscribe(s_down_topic, 1)) {
            ESP_LOGI(TAG, "mqttSubscribe() call could not be queued");
        }
        s_handler_busy = false;
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
        // entry belongs to msg.c, which is the thing that knows
        // which entry a given publish was for. No such entries exist yet.
        break;

    case WALTER_MODEM_MQTT_EVENT_MESSAGE:
        // RTS interlock: net_sleep() runs on modes_run()'s task, which
        // is priority 1 against this task's priority 4, so it gets
        // scheduled every time this handler blocks (mqttReceive()'s AT
        // round trip, ui.c's 10ms disp_wait_busy() poll). Without this
        // flag modes_run() can enter esp_light_sleep_start() - and force
        // RTS high - in the middle of the modem's response to
        // mqttReceive(), i.e. mid-payload rather than at an idle moment.
        // That turns PROTOCOL.md §8.3's "does the Sequans queue or drop
        // when CTS is deasserted" (M5, still UNVERIFIED) from a latency
        // question into a message-loss one. modes_run() ORs
        // net_modem_busy() into its skip_sleep condition.
        s_handler_busy = true;
        if (data->msg_length > PAGER_MAX_PAYLOAD) {
            // F6: oversize payload. Log, count, do not ack/render, but still
            // drain it with a scratch read so it doesn't wedge the modem's
            // buffer into F5.
            s_oversize_count = s_oversize_count + 1; // volatile: avoid deprecated ++ (C++20)
            ESP_LOGI(TAG, "oversize MQTT message dropped: %u bytes > %u cap",
                     (unsigned) data->msg_length, (unsigned) PAGER_MAX_PAYLOAD);
            WalterModem::mqttReceive(data->topic, data->mid, s_mqtt_rx_buf, sizeof(s_mqtt_rx_buf));
            s_handler_busy = false;
            break;
        }

        // L1/L2: never mqttDidRing(). Fetch by the real mid from this event.
        if (!WalterModem::mqttReceive(data->topic, data->mid, s_mqtt_rx_buf, data->msg_length)) {
            ESP_LOGI(TAG, "mqttReceive() failed for mid=%d", data->mid);
            s_handler_busy = false;
            break;
        }

        if (s_msg_cb) {
            s_msg_cb(data->topic, (const char *) s_mqtt_rx_buf, data->msg_length);
        }
        s_handler_busy = false;
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

    // ident's apn is "" for carrier default (ident.h); definePDPContext()
    // wants NULL for that case, not an empty string.
    const char *apn = ident_get_apn();
    if (apn[0] == '\0') {
        apn = nullptr;
    }
    if (!WalterModem::definePDPContext(PAGER_PDP_CTX_ID, apn)) {
        ESP_LOGI(TAG, "definePDPContext() failed");
        return false;
    }

    // Power effect: this is the setting that makes sleep-mode paging cheap;
    // see PROTOCOL.md §8.4 for the (PENDING_HW) current budget it buys.
    if (!WalterModem::configEDRX(WALTER_MODEM_EDRX_ENABLE_WITH_RESULT, PAGER_EDRX_VALUE,
                                 PAGER_EDRX_PTW)) {
        ESP_LOGI(TAG, "configEDRX() failed - continuing without a granted eDRX confirmation");
    }

    // PROTOCOL.md §12 item 6: periodic voltage monitor, ACTIVE mode only
    // (no autonomous shutdown/sleep side effects - modes.c owns all power
    // decisions). Threshold 30 (3.0V) is a sane LiFePO4 low-battery mark;
    // ACTIVE mode never acts on it, it is just recorded alongside the
    // reading. Non-fatal: battery reporting is a nice-to-have, not
    // load-bearing for message delivery, same pattern as configEDRX() above.
    // Power effect: one AT command now, plus one AT round trip per
    // net_get_battery_mv() call later; no RRC of its own either time.
    if (!WalterModem::configVoltageMonitor(WALTER_MODEM_VOLTAGE_MONITOR_MODE_ACTIVE, 30, 30)) {
        ESP_LOGI(TAG, "configVoltageMonitor() failed - continuing without voltage monitoring");
    } else {
        ESP_LOGI(TAG, "voltage monitor configured (ACTIVE, threshold=3.0V, period=30s)");
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
    // DEVICE_PLAN.md §3.3: only rewrite modem NVRAM when ident's ca_hash
    // differs from the hash of what we last wrote this power session (net.cpp
    // today rewrote unconditionally, every net_init() call including every
    // F4 recovery).
    // Power effect: skips one NVRAM write (and its flash wear) per F4
    // recovery once the CA is already current; tlsConfigProfile() below is a
    // cheap AT command and still runs every time.
    const uint8_t *ca_hash = ident_get_ca_hash();
    if (!s_ca_written || memcmp(s_ca_written_hash, ca_hash, IDENT_CA_HASH_LEN) != 0) {
        if (!WalterModem::tlsWriteCredential(false, PAGER_TLS_CA_SLOT, ident_get_ca())) {
            ESP_LOGI(TAG, "tlsWriteCredential() failed");
            return false;
        }
        memcpy(s_ca_written_hash, ca_hash, IDENT_CA_HASH_LEN);
        s_ca_written = true;
        ESP_LOGI(TAG, "CA written to modem slot %u (hash changed)", (unsigned) PAGER_TLS_CA_SLOT);
    } else {
        ESP_LOGD(TAG, "CA unchanged, skipping NVRAM write to slot %u", (unsigned) PAGER_TLS_CA_SLOT);
    }
    if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_CA,
                                       WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT)) {
        ESP_LOGI(TAG, "tlsConfigProfile() failed");
        return false;
    }

    snprintf(s_down_topic, sizeof(s_down_topic), "pager/%s/down", ident_get_dev_id());

    if (!WalterModem::mqttConfig(ident_get_dev_id(), ident_get_dev_id(), ident_get_mqtt_pw(),
                                 PAGER_TLS_PROFILE_ID)) {
        ESP_LOGI(TAG, "mqttConfig() failed");
        return false;
    }

    return true;
}

extern "C" bool net_tls_profile_bootstrap(void)
{
    // DEVICE_PLAN.md §3.2 step 3: the one-time bootstrap MQTT hop
    // (setup.c, F3.5) trusts no CA - the bundle is authenticated and
    // encrypted under a single-use key derived from the typed setup code,
    // so server authentication on this hop would only add DoS resistance
    // (see the rationale quoted in DEVICE_PLAN.md §3.2 step 3). Configures
    // profile PAGER_TLS_BOOTSTRAP_PROFILE_ID only; profile PAGER_TLS_PROFILE_ID
    // (production, CA-pinned to slot PAGER_TLS_CA_SLOT) is untouched.
    // Power effect: one AT command (profile config), no RRC of its own.
    if (!WalterModem::tlsConfigProfile(PAGER_TLS_BOOTSTRAP_PROFILE_ID,
                                       WALTER_MODEM_TLS_VALIDATION_NONE,
                                       WALTER_MODEM_TLS_VERSION_12)) {
        ESP_LOGI(TAG, "tlsConfigProfile(bootstrap) failed");
        return false;
    }
    ESP_LOGI(TAG, "bootstrap TLS profile %d configured (VALIDATION_NONE)",
             PAGER_TLS_BOOTSTRAP_PROFILE_ID);
    return true;
}

extern "C" bool net_session_up(void)
{
    // Power effect: one TLS handshake, ~5kB (PROTOCOL.md §7.2/§7.3), plus
    // the RRC time it takes. Never call this on a timer - only after
    // net_init() and after a detected session loss (F3).
    s_disconnect_edge = false;
    if (!WalterModem::mqttConnect(ident_get_host(), ident_get_port(), PAGER_MQTT_KEEPALIVE_S)) {
        ESP_LOGI(TAG, "mqttConnect() call could not be queued");
        return false;
    }
    ESP_LOGI(TAG, "MQTT connect issued to %s:%u", ident_get_host(), (unsigned) ident_get_port());
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

extern "C" bool net_publish_raw(const char *topic, uint8_t *buf, uint16_t len, uint8_t qos)
{
    if (len > PAGER_MAX_PAYLOAD) {
        ESP_LOGI(TAG, "refusing to publish %u bytes > %u cap (PROTOCOL.md §3.3)",
                 (unsigned) len, (unsigned) PAGER_MAX_PAYLOAD);
        return false;
    }
    // Binary-safe, confirmed by reading the vendor source (not UNVERIFIED):
    // WalterModem::mqttPublish() (src/proto/WalterMQTT.cpp:97-103) puts only
    // the topic string and buf_size on the AT command line
    // ("AT+SQNSMQTTPUBLISH=0,<topic>,<qos>,<buf_size>"); the payload itself
    // is written with uart_write_bytes(_uartNo, cmd->payload, cmd->payloadSize)
    // after the modem's "> " data prompt (src/WalterModem.cpp:2049-2068),
    // i.e. exactly buf_size raw bytes, no NUL-termination or escaping
    // applied to the payload. Same call as net_publish() above, just typed
    // for a CBOR byte buffer instead of a text one.
    return WalterModem::mqttPublish(topic, buf, len, qos);
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

extern "C" bool net_get_battery_mv(int *batt_mv)
{
    // Power effect: one AT round trip ("AT+SQNVMON?" / "+SQNVMON: ..."), no
    // RRC of its own - same class as net_check(). Requires
    // configVoltageMonitor() to have been called once already (net_init()).
    WalterModemRsp rsp = {};
    if (!WalterModem::getVoltage(&rsp)) {
        return false;
    }
    if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_VOLTAGE) {
        return false;
    }
    if (batt_mv) {
        *batt_mv = (int) rsp.data.voltage.voltage * 100; // tenths-of-a-volt -> mV
    }
    return true;
}

extern "C" bool net_get_rssi(int *dbm)
{
    // Power effect: one AT round trip, no RRC of its own - same class as
    // net_check()/net_get_battery_mv(). Vendor call used: getRSSI()
    // (managed_components/dptechnics__walter-modem/src/WalterModem.h:4334),
    // which issues AT+CSQ and converts to dBm itself
    // (src/WalterModem.cpp:4478-4482 issues it; src/WalterModem.cpp:2293
    // does `rsp.data.rssi = -113 + rawRSSI*2`). DEVICE_PLAN.md §5.4 flagged
    // "which of getRSSI()/getSignalQuality() v1.5.0 exposes" as UNVERIFIED,
    // pending 10 min reading WalterModem.h; that reading is done here and
    // settles it: getRSSI()/AT+CSQ is the one that returns a single dBm
    // value in the doc-declared [-113, -51] range, which is what §5.4's
    // bucket table wants - getSignalQuality()/AT+CESQ returns RSRP/RSRQ
    // instead (its WalterModemSignalQuality struct), a different quantity.
    // AT+CSQ's rawRSSI==99 ("not known/not detectable") converts to +85,
    // outside the documented range; treated here as "no reading" rather
    // than fed into the bars table as if it were a 4-bar signal.
    WalterModemRsp rsp = {};
    if (!WalterModem::getRSSI(&rsp)) {
        return false;
    }
    if (rsp.type != WALTER_MODEM_RSP_DATA_TYPE_RSSI) {
        return false;
    }
    if (rsp.data.rssi < -113 || rsp.data.rssi > -51) {
        return false;
    }
    if (dbm) {
        *dbm = rsp.data.rssi;
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

extern "C" bool net_modem_busy(void)
{
    return s_handler_busy;
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
    return ident_get_dev_id();
}

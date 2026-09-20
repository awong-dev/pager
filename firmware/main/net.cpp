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
#include "placeholder_ca.h"

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

// int, not WalterModemState, so this compiles even if the enum ever gains
// values this switch doesn't know about yet -- diagnostic-only, never used
// for control flow.
static const char *walter_state_name(int result)
{
    switch (result) {
    case WALTER_MODEM_STATE_OK: return "OK";
    case WALTER_MODEM_STATE_ERROR: return "ERROR";
    case WALTER_MODEM_STATE_TIMEOUT: return "TIMEOUT";
    case WALTER_MODEM_STATE_NO_MEMORY: return "NO_MEMORY";
    case WALTER_MODEM_STATE_NO_FREE_PDP_CONTEXT: return "NO_FREE_PDP_CONTEXT";
    case WALTER_MODEM_STATE_NO_SUCH_PDP_CONTEXT: return "NO_SUCH_PDP_CONTEXT";
    case WALTER_MODEM_STATE_NO_FREE_SOCKET: return "NO_FREE_SOCKET";
    case WALTER_MODEM_STATE_NO_SUCH_SOCKET: return "NO_SUCH_SOCKET";
    case WALTER_MODEM_STATE_NO_SUCH_PROFILE: return "NO_SUCH_PROFILE";
    case WALTER_MODEM_STATE_BUSY: return "BUSY";
    case WALTER_MODEM_STATE_NO_DATA: return "NO_DATA";
    default: return "?";
    }
}

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

// DEVICE_PLAN.md §3.2 step 3: the one-time bootstrap MQTT hop (setup.c,
// F3.5) reuses PAGER_TLS_PROFILE_ID rather than a separate profile --
// bootstrap and production never run in the same power cycle (main.c only
// reaches setup.c when ident_load() has already failed; setup_run() always
// esp_restart()s on exit), and each path calls tlsConfigProfile() fresh
// before use anyway (VALIDATION_NONE here, VALIDATION_CA in net_init()), so
// there is no real state to collide.
//
// History, for anyone tempted to give this its own profile again: this was
// first 3, which doesn't exist -- the vendor library's own Kconfig caps
// WALTER_MODEM_MAX_TLS_PROFILES at a hard maximum of 3 (`range 1 3`,
// managed_components/dptechnics__walter-modem/Kconfig), so valid IDs are
// only 0/1/2, and every real provisioning attempt failed instantly with
// tlsConfigProfile()'s local NO_SUCH_PROFILE check. Switching to 1 "fixed"
// that check but broke the actual MQTT connect in a much sneakier way: it
// silently hung forever with no CONNECTED/DISCONNECTED event and no
// connection ever reaching the broker (confirmed on real hardware, and
// confirmed NOT a network/SIM/APN issue by testing plain TCP/UDP sockets to
// three different hosts including the vendor's own coap.bluecherry.io,
// which all hung identically). The vendor's own examples/mqtts.cpp names
// the reason directly: "profile 1 is reserved for BlueCherry" -- not a
// style convention, a real modem-side reservation that generic MQTT usage
// silently breaks against.
static constexpr int PAGER_TLS_BOOTSTRAP_PROFILE_ID = PAGER_TLS_PROFILE_ID;

static constexpr uint16_t PAGER_MQTT_KEEPALIVE_S = 1800; // PROTOCOL.md §6.2

static constexpr int PAGER_ATTACH_POLL_CAP_S = 300; // F1: single-attempt cap

// PROTOCOL.md §3.3: hard envelope limit, both directions, for the
// pager/{device_id}/... namespace.
static constexpr uint16_t PAGER_MAX_PAYLOAD = 640;

// PROTOCOL.md §2: the pager/boot/{bid}/... namespace (setup.c, F3.5) has its
// own, larger 4 kB limit for the encrypted bootstrap bundle
// (DEVICE_PLAN.md §3.2 step 4). One shared RX buffer sized to the larger of
// the two; the MESSAGE handler below picks whichever cap applies to the
// topic a given message actually arrived on.
static constexpr uint16_t PAGER_BOOT_MAX_PAYLOAD = 4096;
static constexpr const char *PAGER_BOOT_TOPIC_PREFIX = "pager/boot/";

// DEVICE_PLAN.md §3.2 step 4: the bootstrap session is a few seconds long
// (fetch one retained message, publish one ack, disconnect) — a short
// keepalive is plenty and avoids implying this is a long-lived session.
static constexpr uint16_t PAGER_BOOT_MQTT_KEEPALIVE_S = 60;

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

// Sized to the larger of the two namespace caps above; sized once, no
// malloc on the RX path (L9).
static uint8_t s_mqtt_rx_buf[PAGER_BOOT_MAX_PAYLOAD];

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
        {
            // PROTOCOL.md §2: pager/boot/{bid}/... (setup.c, F3.5) gets the
            // 4 kB bundle cap; every other topic keeps the 640-byte
            // envelope cap. Braced so `payload_cap`'s initialization does
            // not cross into the other case labels below (C++ forbids
            // jumping past a non-trivial initializer within one switch).
            uint16_t payload_cap =
                (strncmp(data->topic, PAGER_BOOT_TOPIC_PREFIX, strlen(PAGER_BOOT_TOPIC_PREFIX)) == 0)
                    ? PAGER_BOOT_MAX_PAYLOAD
                    : PAGER_MAX_PAYLOAD;
            if (data->msg_length > payload_cap) {
                // F6: oversize payload. Log, count, do not ack/render, but
                // still drain it with a scratch read so it doesn't wedge the
                // modem's buffer into F5.
                s_oversize_count = s_oversize_count + 1; // volatile: avoid deprecated ++ (C++20)
                ESP_LOGI(TAG, "oversize MQTT message dropped: %u bytes > %u cap",
                         (unsigned) data->msg_length, (unsigned) payload_cap);
                WalterModem::mqttReceive(data->topic, data->mid, s_mqtt_rx_buf, sizeof(s_mqtt_rx_buf));
                s_handler_busy = false;
                break;
            }
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
// v0.2 bug fix #4 (docs/V02_DESIGN.md §2.4, "Empty CA slot"). UNVERIFIED
// whether the MQTT engine does TLS at all when the cert slot a profile names
// is *empty* -- BRINGUP_NOTES.md only confirmed that a profile which does
// not name a slot at all silently falls back to a plaintext CONNECT. Every
// TLS profile this file configures for MQTT already names PAGER_TLS_CA_SLOT
// even with validation off, so the "unnamed" case cannot happen here; this
// closes the "named but empty" one, which a factory-fresh modem or a device
// whose identity has never pinned a CA (DEVICE_PLAN.md §3.3: "the CA is
// optional") can hit. Writing the ISRG Root X2 placeholder (placeholder_ca.h)
// makes the slot never actually empty. It is never validated against: the
// only callers of this function are branches that are about to configure
// (or already run) WALTER_MODEM_TLS_VALIDATION_NONE.
// ---------------------------------------------------------------------------
static void ensure_ca_slot_populated(void)
{
    if (s_ca_written) {
        return; // a real CA or the placeholder was already written this power session
    }
    if (ident_get_slot12_populated()) {
        // Known good from a prior boot (NVS "ident"/"slot12") - one NVRAM
        // write per device lifetime in practice, per the design's own note.
        s_ca_written = true;
        return;
    }
    if (!WalterModem::tlsWriteCredential(false, PAGER_TLS_CA_SLOT, PAGER_PLACEHOLDER_CA_PEM)) {
        ESP_LOGI(TAG, "failed to write placeholder CA to slot %u - MQTT TLS behaviour with an "
                      "empty named slot stays UNVERIFIED (BRINGUP_NOTES.md)",
                 (unsigned) PAGER_TLS_CA_SLOT);
        return;
    }
    s_ca_written = true;
    ident_set_slot12_populated();
    ESP_LOGI(TAG, "placeholder CA (ISRG Root X2) written to modem slot %u - never validated "
                  "against, only keeps the slot non-empty",
             (unsigned) PAGER_TLS_CA_SLOT);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" bool net_init(void)
{
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
    // Debug builds only (main/CMakeLists.txt): raw AT TX:/RX: trace, so URCs
    // such as +SQNSMQTTONMESSAGE are visible on the console.
    esp_log_level_set("WalterModem", ESP_LOG_DEBUG);
#endif
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
    //
    // Sanity window, found live: right after attach the modem's RTC can still
    // be unset and AT+CCLK? answers "70/01/01,00:00:08". The library reads the
    // two-digit year as 20YY, giving 2070 (epoch 3155760008), and the relay
    // rejects every envelope carrying it ("ts out of range"). NITZ normally
    // lands within a few seconds of attach, so retry briefly, and fall back
    // to the documented ts:0 rather than ever publishing a bogus time.
    static constexpr int64_t PAGER_CLOCK_MIN = 1704067200LL; // 2024-01-01
    static constexpr int64_t PAGER_CLOCK_MAX = 3124224000LL; // 2069-01-01, below the "70" artefact
    s_clock_epoch = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        WalterModemRsp rsp = {};
        if (WalterModem::getClock(&rsp)) {
            int64_t t = rsp.data.clock.epochTime;
            if (t >= PAGER_CLOCK_MIN && t < PAGER_CLOCK_MAX) {
                s_clock_epoch = t;
                s_clock_epoch_us = esp_timer_get_time();
                break;
            }
            ESP_LOGI(TAG, "modem clock not set yet (epoch=%lld), retrying", (long long) t);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_clock_epoch != 0) {
        ESP_LOGI(TAG, "clock seeded from network: epoch=%lld", (long long) s_clock_epoch);
    } else {
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
    // A CA is pinned only when the bundle carried one. With none, the session
    // runs with validation off: envelopes are still HMAC-authenticated
    // (PROTOCOL.md section 2.4), and a broker that changes its root CA can no longer
    // brick the pager. Either way the CA slot MUST be named in the profile --
    // see net_tls_profile_bootstrap() for the plaintext-fallback finding.
    const bool pin_ca = ident_get_ca_len() > 0;
    if (pin_ca) {
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
    } else {
        // v0.2 bug fix #4 (§2.4): no CA pinned -- this is exactly the
        // VALIDATION_NONE branch below, so make sure slot PAGER_TLS_CA_SLOT
        // is not left empty (UNVERIFIED what the MQTT engine does then).
        ensure_ca_slot_populated();
    }
    if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID,
                                       pin_ca ? WALTER_MODEM_TLS_VALIDATION_CA
                                              : WALTER_MODEM_TLS_VALIDATION_NONE,
                                       WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT)) {
        ESP_LOGI(TAG, "tlsConfigProfile() failed");
        return false;
    }
    ESP_LOGI(TAG, "TLS profile %d: %s", PAGER_TLS_PROFILE_ID,
             pin_ca ? "CA pinned (VALIDATION_CA)" : "no CA pinned (VALIDATION_NONE)");

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
    //
    // The CA slot MUST be named even though validation is off. Confirmed on
    // real hardware (GM02SP LR8.2.1.0-61488) by capturing the wire bytes on a
    // server we control: with AT+SQNSPCFG=2,2,"",0,,,, (no CA slot) the
    // modem's AT+SQNSMQTT* engine silently skips TLS and sends a PLAINTEXT
    // MQTT CONNECT -- credentials included -- to the TLS port. A TLS-only
    // broker then waits for a ClientHello forever and +SQNSMQTTONCONNECT
    // never fires (the original `setup` hang). With the slot named
    // (AT+SQNSPCFG=2,2,"",0,12,,,) the same engine sends a normal TLS 1.2
    // ClientHello with SNI. The generic socket layer (AT+SQNSD) does TLS
    // either way, which is why nettest never reproduced this.
    // v0.2 bug fix #4 (§2.4): behaviour when slot PAGER_TLS_CA_SLOT is empty
    // (as on a factory-fresh modem -- every test so far had a cert in it)
    // was UNVERIFIED and stayed that way; this makes it not matter, the
    // same way net_init()'s own VALIDATION_NONE branch does.
    ensure_ca_slot_populated();

    WalterModemRsp rsp = {};
    if (!WalterModem::tlsConfigProfile(PAGER_TLS_BOOTSTRAP_PROFILE_ID,
                                       WALTER_MODEM_TLS_VALIDATION_NONE,
                                       WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT, 0xff, 0xff,
                                       &rsp)) {
        ESP_LOGI(TAG, "tlsConfigProfile(bootstrap) failed (result=%s)",
                 walter_state_name(rsp.result));
        return false;
    }
    ESP_LOGI(TAG, "bootstrap TLS profile %d configured (VALIDATION_NONE)",
             PAGER_TLS_BOOTSTRAP_PROFILE_ID);
    return true;
}

// ---------------------------------------------------------------------------
// Bootstrap-only additions (docs/DEVICE_PLAN.md §3.2, setup.c F3.5). See
// net.h's module note on why these live here despite not being in F3.5's
// `Files` list: WalterModem is a C++-only API, and net_init()/net_session_up()
// are hardwired to ident's production values, which do not exist yet during
// a bootstrap fetch.
// ---------------------------------------------------------------------------

extern "C" bool net_bootstrap_attach(const char *apn)
{
    // Power effect: same class as net_init()'s attach phase — modem leaves
    // reset, attaches LTE-M. No eDRX/PSM/voltage-monitor requested (this
    // session is torn down within seconds, see net.h's doc comment).
    if (!WalterModem::begin(PAGER_MODEM_UART)) {
        ESP_LOGI(TAG, "WalterModem::begin() failed (bootstrap)");
        return false;
    }

    WalterModem::setMQTTEventHandler(pager_mqtt_event_handler, nullptr);
    WalterModem::setNetworkEventHandler(pager_network_event_handler, nullptr);

    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGI(TAG, "setOpState(NO_RF) failed (bootstrap)");
        return false;
    }

    // DEVICE_PLAN.md §3.2 step 2: "the code's APN or the carrier default" —
    // same NULL-for-empty convention net_init() uses for ident's apn.
    const char *use_apn = (apn && apn[0] != '\0') ? apn : nullptr;
    if (!WalterModem::definePDPContext(PAGER_PDP_CTX_ID, use_apn)) {
        ESP_LOGI(TAG, "definePDPContext() failed (bootstrap)");
        return false;
    }

    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        ESP_LOGI(TAG, "setOpState(FULL) failed (bootstrap)");
        return false;
    }

    if (!WalterModem::setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        ESP_LOGI(TAG, "setNetworkSelectionMode() failed (bootstrap)");
        return false;
    }

    // Same single-attempt cap as net_init()'s F1 attach wait; the caller
    // (setup.c) owns turning a false return into the "no network" message
    // DEVICE_PLAN.md §3.2 step 5 names.
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
        ESP_LOGI(TAG, "network attach timed out after %ds (bootstrap)", PAGER_ATTACH_POLL_CAP_S);
        return false;
    }
    ESP_LOGI(TAG, "network attached (bootstrap)");

    // TEMPORARY diagnostic: real-hardware bootstrap MQTT connect stalled
    // silently for the full 30s poll with no CONNECTED/DISCONNECTED event,
    // and EMQX Cloud's own dashboard shows no record of a connection
    // attempt ever arriving -- meaning the failure is somewhere between the
    // modem and the broker, not a broker-side rejection. Confirming the PDP
    // context actually has a real IP address rules in/out "attached at the
    // RRC/LTE level but never got real IP connectivity" as the cause.
    WalterModemRsp pdp_rsp = {};
    if (WalterModem::getPDPAddress(&pdp_rsp, NULL, NULL, PAGER_PDP_CTX_ID)) {
        ESP_LOGI(TAG, "PDP address (bootstrap): %s / %s",
                 pdp_rsp.data.pdpAddressList.pdpAddress ? pdp_rsp.data.pdpAddressList.pdpAddress : "(null)",
                 pdp_rsp.data.pdpAddressList.pdpAddress2 ? pdp_rsp.data.pdpAddressList.pdpAddress2 : "(null)");
    } else {
        ESP_LOGI(TAG, "getPDPAddress() failed (bootstrap, result=%s)", walter_state_name(pdp_rsp.result));
    }

    return true;
}

extern "C" bool net_bootstrap_connect(const char *client_id, const char *password, const char *host,
                                      uint16_t port, const char *down_topic)
{
    // Points the shared down-topic buffer at the bootstrap topic so the
    // existing CONNECTED-event auto-resubscribe (pager_mqtt_event_handler
    // above) subscribes to it, exactly like the production session does for
    // pager/{own_id}/down — see net.h's module note on why that sharing is
    // safe here (bootstrap and production never coexist in one power cycle).
    snprintf(s_down_topic, sizeof(s_down_topic), "%s", down_topic);

    if (!WalterModem::mqttConfig(client_id, client_id, password,
                                 PAGER_TLS_BOOTSTRAP_PROFILE_ID)) {
        ESP_LOGI(TAG, "mqttConfig() failed (bootstrap)");
        return false;
    }

    // Power effect: one TLS handshake (~5 kB, VALIDATION_NONE profile, no CA
    // round trip) plus the RRC time it takes.
    s_disconnect_edge = false;
    if (!WalterModem::mqttConnect(host, port, PAGER_BOOT_MQTT_KEEPALIVE_S)) {
        ESP_LOGI(TAG, "mqttConnect() call could not be queued (bootstrap)");
        return false;
    }
    ESP_LOGI(TAG, "bootstrap MQTT connect issued to %s:%u as %s", host, (unsigned) port, client_id);
    return true;
}

extern "C" bool net_write_ca(const char *ca_pem)
{
    // DEVICE_PLAN.md §3.2 step 4's "CA to slot 12": same slot/call net_init()
    // uses for the production CA, called unconditionally here (no ca_hash
    // short-circuit — this runs at most once per device lifetime, unlike
    // net_init()'s per-boot/per-F4-recovery calls).
    // Power effect: one NVRAM write on the modem's own storage, no RRC.
    if (!WalterModem::tlsWriteCredential(false, PAGER_TLS_CA_SLOT, ca_pem)) {
        ESP_LOGI(TAG, "tlsWriteCredential() failed (bootstrap CA write)");
        return false;
    }
    ESP_LOGI(TAG, "CA written to modem slot %u (bootstrap)", (unsigned) PAGER_TLS_CA_SLOT);
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

extern "C" bool net_check_sim(void)
{
    if (!WalterModem::begin(PAGER_MODEM_UART)) {
        ESP_LOGI(TAG, "SIM check: WalterModem::begin() failed");
        return false;
    }
    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGI(TAG, "SIM check: setOpState(NO_RF) failed");
        return false;
    }

    // First real caller of getSIMCardIMSI() in this codebase (net_init()'s
    // own production bring-up never reads the SIM directly) -- no prior
    // evidence either way on how quickly this modem/SIM combination
    // actually answers AT+CIMI right after a CFUN=4 transition. Real SIM
    // cards commonly need a brief moment to power up and become readable
    // after the interface that reads them is (re)enabled; a single
    // immediate attempt has no way to tell "genuinely no SIM" apart from
    // "SIM just needed another moment", so this polls for up to
    // PAGER_SIM_CHECK_POLL_CAP_S rather than firing once. Power effect:
    // same AT-round-trip class as a single attempt, repeated at most this
    // many times, no RRC either way (opstate is NO_RF throughout).
    constexpr int PAGER_SIM_CHECK_POLL_CAP_S = 5;
    WalterModemRsp rsp = {};
    bool ok = false;
    for (int waited_s = 0; waited_s <= PAGER_SIM_CHECK_POLL_CAP_S; waited_s++) {
        rsp = {};
        ok = WalterModem::getSIMCardIMSI(&rsp);
        if (ok) {
            break;
        }
        ESP_LOGI(TAG, "SIM check: attempt %d/%d failed (result=%s)", waited_s + 1,
                 PAGER_SIM_CHECK_POLL_CAP_S + 1, walter_state_name(rsp.result));
        if (waited_s < PAGER_SIM_CHECK_POLL_CAP_S) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "SIM check: %s", ok ? "IMSI read OK" : "no SIM detected (IMSI read failed)");
    return ok;
}

extern "C" bool net_check_tcp(const char *host, uint16_t port, bool udp, bool tls)
{
    if (!net_bootstrap_attach(NULL)) {
        ESP_LOGI(TAG, "nettest: attach failed");
        return false;
    }

    if (tls) {
        if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_NONE,
                                           WALTER_MODEM_TLS_VERSION_12)) {
            ESP_LOGI(TAG, "nettest: tlsConfigProfile() failed");
            return false;
        }
    }

    constexpr int PAGER_TCP_TEST_SOCKET_ID = 1;
    if (!WalterModem::socketConfig(PAGER_TCP_TEST_SOCKET_ID)) {
        ESP_LOGI(TAG, "nettest: socketConfig() failed");
        return false;
    }
    // Confirmed missing here by comparison against the vendor's own
    // examples/udp and examples/tcp: both call socketConfigSecure(id,
    // false) right after socketConfig(), before ever dialing -- every
    // socket dial attempt without this hung identically at
    // WALTER_MODEM_SOCKET_STATE_PENDING_NO_DATA(5) forever, TCP or UDP, to
    // three different hosts including the vendor's own coap.bluecherry.io
    // (reachable fine via the modem's separate, dedicated BlueCherry/CoAP
    // client), which is what proved this was a socket-config bug rather
    // than a SIM/APN restriction.
    //
    // tls option: added after a real MQTT bootstrap connect (TLS, profile
    // 2, VALIDATION_NONE) hung the exact same way against a host:port a
    // plaintext nettest just proved reachable -- isolates "is it TLS
    // itself" from "is it the MQTT protocol layer" by wrapping this same
    // generic socket (not MQTT at all) in TLS profile PAGER_TLS_PROFILE_ID.
    if (!WalterModem::socketConfigSecure(PAGER_TCP_TEST_SOCKET_ID, tls, PAGER_TLS_PROFILE_ID)) {
        ESP_LOGI(TAG, "nettest: socketConfigSecure() failed");
        return false;
    }

    WalterModemRsp rsp = {};
    WalterModemSocketProto proto = udp ? WALTER_MODEM_SOCKET_PROTO_UDP : WALTER_MODEM_SOCKET_PROTO_TCP;
    if (!WalterModem::socketDial(PAGER_TCP_TEST_SOCKET_ID, proto, port, host, 0,
                                 WALTER_MODEM_ACCEPT_ANY_REMOTE_DISABLED, &rsp)) {
        ESP_LOGI(TAG, "nettest: socketDial() failed (result=%s)", walter_state_name(rsp.result));
        return false;
    }
    ESP_LOGI(TAG, "nettest: socketDial (%s) issued to %s:%u -- OK response means dialed",
             udp ? "UDP" : "TCP", host, (unsigned) port);

    // Confirmed by comparison against the working vendor reference
    // (examples/walter_feels, verified live against this exact SIM/host):
    // it NEVER polls socketGetState() before sending -- it treats
    // socketDial()'s own "OK" as sufficient and sends immediately. AT+SQNSS?
    // on our own socket right after a successful dial showed a fully
    // resolved 5-tuple (real remote IP, real local/remote ports) at
    // "status 2" (WALTER_MODEM_SOCKET_STATE_PENDING_NO_DATA once mapped) --
    // this diagnostic's earlier "poll for OPENED/READY" loop was waiting for
    // a state a live, working UDP socket apparently never reaches, producing
    // a false "FAILED" on a socket that was actually fine. Sending a real
    // payload and getting AT-level "OK" back is a much more direct test of
    // whether the socket actually works than guessing at the right
    // state enum.
    const char *test_payload = "pager nettest";
    bool sent = WalterModem::socketSend(PAGER_TCP_TEST_SOCKET_ID, (uint8_t *) test_payload,
                                        (uint16_t) strlen(test_payload));
    ESP_LOGI(TAG, "nettest: socketSend: %s", sent ? "OK" : "FAILED");
    WalterModem::socketClose(PAGER_TCP_TEST_SOCKET_ID);
    return sent;
}

extern "C" bool net_check_mqtt(const char *host, uint16_t port, int tls_mode)
{
    // TEMPORARY diagnostic (main.c's `mqtttest`): points the modem's own
    // AT+SQNSMQTT* engine at an arbitrary host:port over the VALIDATION_NONE
    // bootstrap TLS profile, so a TLS server under our control (e.g.
    // `openssl s_server -tlsextdebug -msg`) can show exactly what ClientHello
    // the MQTT engine sends -- SNI present or not, TLS version, ciphers.
    // Dummy credentials: the far end need not be a real broker.
    if (!net_bootstrap_attach(NULL)) {
        ESP_LOGI(TAG, "mqtttest: attach failed");
        return false;
    }
    if (!net_tls_profile_bootstrap()) {
        return false;
    }
    // tls_mode: 0 = bootstrap profile as-is (VALIDATION_NONE, no CA slot),
    // 1 = VALIDATION_CA + CA slot, 2 = VALIDATION_NONE + CA slot, 3 = mode 2
    // after first deleting the cert in that slot (factory-fresh case). Mode 2
    // separates "validation level 0" from "no CA slot named" as the thing
    // that makes the engine fall back to plaintext.
    if (tls_mode == 3) {
        // `emptyca`: reproduce a factory-fresh modem. Writing zero bytes to a
        // credential slot deletes it (Sequans AT+SQNSNVW). DESTRUCTIVE to slot
        // PAGER_TLS_CA_SLOT -- harmless once no CA is pinned, and net_init()
        // rewrites the slot on the next boot if the identity does pin one.
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "AT+SQNSNVW=\"certificate\",%u,0", (unsigned) PAGER_TLS_CA_SLOT);
        bool del = WalterModem::sendCmd(cmd);
        ESP_LOGI(TAG, "mqtttest: delete cert slot %u: %s", (unsigned) PAGER_TLS_CA_SLOT,
                 del ? "OK" : "FAILED (slot may already be empty)");
        s_ca_written = false;
    }
    if (tls_mode == 2 || tls_mode == 3) {
        if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_NONE,
                                           WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT)) {
            ESP_LOGI(TAG, "mqtttest: tlsConfigProfile(VALIDATION_NONE + CA slot) failed");
            return false;
        }
    }
    if (tls_mode == 1) {
        // `mqtttest <host> <port> ca`: same profile id, but configured the way
        // the vendor's examples/mqtts and net_init() do it (VALIDATION_CA with
        // the CA slot). Exists because the VALIDATION_NONE profile was observed
        // to make the MQTT engine send a PLAINTEXT CONNECT to the TLS port --
        // this isolates whether the validation level is what disables TLS.
        if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_CA,
                                           WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT)) {
            ESP_LOGI(TAG, "mqtttest: tlsConfigProfile(VALIDATION_CA) failed");
            return false;
        }
    }
    if (!net_bootstrap_connect("pager-sni-test", "x", host, port, "pager/sni-test/down")) {
        return false;
    }

    // Wait up to 30 s for any outcome. Against a non-MQTT TLS server the
    // expected result is "no event" -- the server-side log is the real output.
    for (int i = 0; i < 30; i++) {
        if (s_mqtt_connected) {
            ESP_LOGI(TAG, "mqtttest: MQTT session usable after ~%d s", i);
            return true;
        }
        if (s_disconnect_edge) {
            ESP_LOGI(TAG, "mqtttest: connect failed / disconnected after ~%d s", i);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!s_disconnect_edge) {
        ESP_LOGI(TAG, "mqtttest: no CONNECTED/DISCONNECTED event within 30 s");
    }
    WalterModem::mqttDisconnect();
    return false;
}

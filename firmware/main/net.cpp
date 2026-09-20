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
#include "carrier.h"
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

// v0.2 §4.4 (CA trust, cafetch.c): profile 3 is reserved for the CA fetch
// per docs/V02_DESIGN.md §1 patch 3 / PATCHES.md 1.3. Socket id 4 is chosen
// simply to sit clear of nettest's own PAGER_TCP_TEST_SOCKET_ID (1) — the
// vendor's WALTER_MODEM_MAX_SOCKETS default is 6 (ids 1-6), so both fit
// comfortably with no risk of colliding with a diagnostic run left active.
static constexpr int PAGER_CA_FETCH_TLS_PROFILE_ID = 3;
static constexpr int PAGER_CA_FETCH_SOCKET_ID = 4;

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

// v0.2 §5 (location, loc.c): GNSS event handoff, same single-flag pattern as
// the msg-callback path above. Written only by pager_gnss_event_handler()
// (WalterModem's _eventProcessingTask), read/cleared only by
// net_gnss_poll_event() (loc.c's own task, via loc_service()) -- one flag,
// one writer, one reader, same reasoning s_msg_cb's buffer already relies on.
static volatile bool s_gnss_event_pending = false;
static net_gnss_event_t s_gnss_event;

// v0.2 §6 (device-direct SMS, sms.c): same single-flag event handoff
// pattern as s_gnss_event_pending above -- written only by
// pager_sms_event_handler() (WalterModem's _eventProcessingTask), read/
// cleared only by net_sms_poll_event() (sms.c's own task, via
// sms_service() from modes_run()).
static volatile bool s_sms_event_pending = false;
static net_sms_event_t s_sms_event;

// v0.2 §4.4 (CA trust, cafetch.c): same single-flag event handoff pattern as
// s_gnss_event_pending above -- set only by pager_socket_event_handler()
// (WalterModem's own _eventProcessingTask), read/cleared only by
// net_ca_fetch_poll() (cafetch.c's own caller, via catrust_service() from
// modes_run()'s task). Only ever meaningful for PAGER_CA_FETCH_SOCKET_ID:
// this project has exactly one other socket user (nettest's
// PAGER_TCP_TEST_SOCKET_ID), which does not register for socket events at
// all (it polls send()'s own AT-level OK/FAILED instead), so filtering by
// conn_id in the handler is defense in depth, not load-bearing today.
static volatile bool s_ca_fetch_ring_pending = false;
static volatile bool s_ca_fetch_closed = false;

// Cell-change trigger (V02_DESIGN.md §5 trigger 1): the callback loc.c
// registers, plus the last "lac:ci" key seen, so this file only invokes the
// callback on a genuine change (loc.c's own 10-minute debounce decides
// whether that change actually resets the backoff -- see net_set_cell_change_cb()'s
// own doc comment in net.h).
static void (*s_cell_change_cb)(const char *) = nullptr;
// Sized for the worst case ("lac"/"ci" are each up to 15 chars + NUL in the
// vendor's own WMNetworkEventData, WalterModem.h) plus the ":" separator and
// NUL, so snprintf() below can never truncate -- loc.c's own cell_key field
// (LOC_CELL_KEY_MAX, loc.h) is smaller and truncates via strncpy() instead,
// which is fine there (an opaque comparison key, never rendered).
static char s_last_cell_key[40] = { 0 };
static bool s_have_last_cell_key = false;

// v0.2 §5: arms IO2 (LIS3DH INT1) as an ext1 light-sleep wake source, only
// once accel.c has confirmed the chip is actually present (see
// net_enable_accel_wake()'s own doc comment in net.h).
static bool s_accel_wake_enabled = false;

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

// v0.2 §5 (location): GNSS event handoff. Runs on WalterModem's
// _eventProcessingTask (same task pager_mqtt_event_handler above runs on) --
// per this task's own rule ("never call modem APIs from the GNSS event
// callback beyond what the library's own examples do"), this function does
// nothing but classify the vendor's WMGNSSFixEvent and copy it into
// s_gnss_event; every subsequent modem call (cancel, re-attach, ...) happens
// from loc.c's own task via net_gnss_poll_event() and the rest of this
// file's net_gnss_*()/net_radio_*() functions.
static void pager_gnss_event_handler(WMGNSSEventType event, const WMGNSSEventData *data, void *args)
{
    (void) args;
    if (event != WALTER_MODEM_GNSS_EVENT_FIX) {
        return; // STATUS/ASSISTANCE URCs: nothing in this design consumes them yet
    }
    const WMGNSSFixEvent *fix = &data->gnssfix;
    net_gnss_event_t ev = {};
    switch (fix->status) {
    case WALTER_MODEM_GNSS_FIX_STATUS_READY:
        ev.kind = NET_GNSS_EVT_FIX;
        ev.lat = fix->latitude;
        ev.lon = fix->longitude;
        ev.confidence = fix->estimatedConfidence;
        ev.fix_ts = fix->timestamp;
        ev.sat_count = fix->satCount;
        break;
    case WALTER_MODEM_GNSS_FIX_STATUS_LTE_CONCURRENCY:
        ev.kind = NET_GNSS_EVT_REFUSED;
        break;
    case WALTER_MODEM_GNSS_FIX_STATUS_STOPPED_BY_USER:
    case WALTER_MODEM_GNSS_FIX_STATUS_NO_RTC:
    default:
        ev.kind = NET_GNSS_EVT_NO_FIX;
        break;
    }
    s_gnss_event = ev;
    s_gnss_event_pending = true;
}

// v0.2 §6 (device-direct SMS): `+CMTI` new-message event handoff. Runs on
// WalterModem's _eventProcessingTask (same task pager_mqtt_event_handler/
// pager_gnss_event_handler above run on) -- per V02_DESIGN.md §6's own rule
// ("never do modem work in the event callback beyond what the library's own
// patterns allow"), this function does nothing but copy the vendor's
// WMSmsEventData into s_sms_event; the actual net_sms_read()/
// net_sms_delete() calls happen from sms.c's own task via
// net_sms_poll_event() and sms_service(), never from here.
static void pager_sms_event_handler(WMSmsEventType event, const WMSmsEventData *data, void *args)
{
    (void) args;
    if (event != WALTER_MODEM_SMS_EVENT_RING) {
        return;
    }
    s_sms_event.index = data->index;
    strncpy(s_sms_event.mem, data->mem, sizeof(s_sms_event.mem) - 1);
    s_sms_event.mem[sizeof(s_sms_event.mem) - 1] = '\0';
    s_sms_event_pending = true;
}

// v0.2 §4.4 (CA trust, cafetch.c): runs on WalterModem's _eventProcessingTask
// (same task pager_mqtt_event_handler/pager_gnss_event_handler above run
// on) -- per this task's own rule, does nothing but latch a flag; every
// subsequent modem call (socketReceive(), socketClose()) happens from
// cafetch.c's own caller via net_ca_fetch_poll()/net_ca_fetch_close(), never
// from here.
static void pager_socket_event_handler(WMSocketEventType event, const WMSocketEventData *data, void *args)
{
    (void) args;
    if (data->conn_id != PAGER_CA_FETCH_SOCKET_ID) {
        return; // not our socket (nettest's own socket does not use this event path)
    }
    switch (event) {
    case WALTER_MODEM_SOCKET_EVENT_RING:
        s_ca_fetch_ring_pending = true;
        break;
    case WALTER_MODEM_SOCKET_EVENT_DISCONNECTED:
        s_ca_fetch_closed = true;
        break;
    }
}

static void pager_network_event_handler(WMNetworkEventType event, const WMNetworkEventData *data, void *args)
{
    (void) args;

    if (event == WALTER_MODEM_NETWORK_EVENT_REG_STATE_CHANGE) {
        ESP_LOGI(TAG, "network registration state -> %d", (int) data->cereg.state);
        // v0.2 §5 trigger 1: lac/ci are only non-empty when the modem's
        // CEREG report type carries location info (net_init() requests
        // ENABLED_WITH_LOCATION) -- empty on a plain state change (e.g. the
        // deregistration this file's own route-2 NO_RF transition causes),
        // which this guard correctly ignores rather than treating "no
        // service" as a cell change.
        if (data->cereg.lac[0] != '\0' && data->cereg.ci[0] != '\0') {
            char key[sizeof(s_last_cell_key)];
            snprintf(key, sizeof(key), "%s:%s", data->cereg.lac, data->cereg.ci);
            if (!s_have_last_cell_key || strncmp(key, s_last_cell_key, sizeof(key)) != 0) {
                strncpy(s_last_cell_key, key, sizeof(s_last_cell_key) - 1);
                s_last_cell_key[sizeof(s_last_cell_key) - 1] = '\0';
                s_have_last_cell_key = true;
                if (s_cell_change_cb) {
                    s_cell_change_cb(s_last_cell_key);
                }
            }
        }
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

// Reads what automatic carrier detection needs from the SIM: the IMSI and
// EF_GID1 (file 0x6F3E = 28478) as hex. The SIM must be powered (CFUN 1 or 4)
// and, as net_check_sim() found, may need a moment after CFUN=4.
// Power effect: a handful of AT round trips, no RRC. Runs once per attach and
// only in automatic mode.
static bool read_sim_identity(char *imsi, size_t imsi_cap, char *gid1_hex, size_t gid_cap)
{
    imsi[0] = '\0';
    gid1_hex[0] = '\0';
    for (int attempt = 0; attempt < 6 && imsi[0] == '\0'; attempt++) {
        WalterModemRsp rsp = {};
        if (WalterModem::getSIMCardIMSI(&rsp) && rsp.data.imsi[0] != '\0') {
            snprintf(imsi, imsi_cap, "%s", rsp.data.imsi);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (imsi[0] == '\0') {
        return false;
    }
    // EF_GID1's length is operator-defined and a READ BINARY with the wrong
    // length fails (sw1 0x67), so try the plausible lengths, longest first.
    static const int k_lens[] = { 16, 8, 4, 2, 1 };
    for (size_t i = 0; i < sizeof(k_lens) / sizeof(k_lens[0]); i++) {
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+CRSM=176,28478,0,0,%d", k_lens[i]);
        if (!WalterModem::sendCmd(cmd)) {
            continue; // +CME ERROR: the file does not exist on this SIM
        }
        const char *r = WalterModem::simLastCRSM(); // "+CRSM: 144,0,20FF"
        int sw1 = 0, sw2 = 0;
        char hex[40] = "";
        if (sscanf(r, "+CRSM: %d,%d,%39[0-9A-Fa-f]", &sw1, &sw2, hex) >= 2 && sw1 == 144 && hex[0]) {
            snprintf(gid1_hex, gid_cap, "%s", hex);
            break;
        }
    }
    return true;
}

// Which APN to attach with. See carrier.h for the precedence and why a blank
// APN is not a safe default. `typed` is an APN given explicitly as part of a
// typed setup code; `stored` is the one the setup bundle carried (ident).
static const char *effective_apn(const char *typed, const char *stored)
{
    static char s_detected_apn[CARRIER_APN_MAX];
    const char *apn = nullptr;
    const char *why = "network's choice (blank)";
    if (typed && typed[0] != '\0') {
        apn = typed;
        why = "typed with the setup code";
    } else if (carrier_get_mode() == CARRIER_MODE_FIXED) {
        apn = carrier_get_apn()[0] ? carrier_get_apn() : nullptr;
        why = carrier_get_label();
    } else {
        char imsi[20], gid1[40];
        const carrier_preset_t *p = nullptr;
        if (read_sim_identity(imsi, sizeof(imsi), gid1, sizeof(gid1))) {
            p = carrier_detect(imsi, gid1);
            ESP_LOGI(TAG, "SIM: network %.6s, GID1 %s -> %s", imsi, gid1[0] ? gid1 : "(none)",
                     p ? p->label : "not in the carrier table");
        } else {
            ESP_LOGI(TAG, "SIM identity could not be read; no automatic APN");
        }
        carrier_note_detected(p ? p->label : "");
        if (p) {
            snprintf(s_detected_apn, sizeof(s_detected_apn), "%s", p->apn);
            apn = s_detected_apn[0] ? s_detected_apn : nullptr;
            why = "detected from the SIM";
        } else if (stored && stored[0] != '\0') {
            apn = stored;
            why = "from the setup bundle";
        }
    }
    ESP_LOGI(TAG, "APN: '%s' (%s)", apn ? apn : "", why);
    return apn;
}

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
    WalterModem::setGNSSEventHandler(pager_gnss_event_handler, nullptr); // v0.2 §5
    WalterModem::setSocketEventHandler(pager_socket_event_handler, nullptr); // v0.2 §4.4
    // v0.2 §6: registered unconditionally, same as the handlers above --
    // harmless even if sms_init()'s own net_sms_config() later fails/is
    // never called (a `+CMTI` URC cannot arrive if AT+CNMI was never
    // configured, so this registration alone has no observable effect
    // until smsConfig() succeeds).
    WalterModem::setSmsEventHandler(pager_sms_event_handler, nullptr);

    // v0.2 §5 trigger 1 (cell/tracking-area change): the default CEREG
    // report type carries no lac/ci at all, so pager_network_event_handler()
    // would never see a cell identity to compare. Non-fatal: the trigger
    // simply never fires if this is rejected (this design's own fail-open
    // rule for everything location-related), same tolerance configEDRX()
    // below already documents for a rejected request.
    if (!WalterModem::configCEREGReports(WALTER_MODEM_CEREG_REPORTS_ENABLED_WITH_LOCATION)) {
        ESP_LOGI(TAG, "configCEREGReports(ENABLED_WITH_LOCATION) failed - cell-change location "
                      "trigger will never fire");
    }

    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGI(TAG, "setOpState(NO_RF) failed");
        return false;
    }

    // ident's apn is "" for carrier default (ident.h); definePDPContext()
    // wants NULL for that case, not an empty string.
    const char *apn = effective_apn(nullptr, ident_get_apn());
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
    const char *use_apn = effective_apn(apn, nullptr);
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

extern "C" bool net_write_ca_slot(uint8_t slot, const char *ca_pem)
{
    // v0.2 §4.4: generalises the bootstrap-only net_write_ca() below so
    // catrust.c's two-phase apply can target the scratch slot too.
    // Power effect: one NVRAM write on the modem's own storage, no RRC.
    if (!WalterModem::tlsWriteCredential(false, slot, ca_pem)) {
        ESP_LOGI(TAG, "tlsWriteCredential() failed (slot %u)", (unsigned) slot);
        return false;
    }
    ESP_LOGI(TAG, "CA written to modem slot %u", (unsigned) slot);
    return true;
}

extern "C" bool net_write_ca(const char *ca_pem)
{
    // DEVICE_PLAN.md §3.2 step 4's "CA to slot 12": same slot/call net_init()
    // uses for the production CA, called unconditionally here (no ca_hash
    // short-circuit — this runs at most once per device lifetime, unlike
    // net_init()'s per-boot/per-F4-recovery calls).
    return net_write_ca_slot(PAGER_TLS_CA_SLOT, ca_pem);
}

extern "C" bool net_tls_configure(uint8_t ca_slot, bool validated)
{
    // v0.2 §4.2/§4.4: reconfigures the PRODUCTION profile (PAGER_TLS_PROFILE_ID)
    // to name `ca_slot`, validated or not -- ALWAYS naming a slot (never the
    // BRINGUP_NOTES.md plaintext-fallback shape). Power effect: one AT
    // command, no RRC of its own.
    if (!WalterModem::tlsConfigProfile(PAGER_TLS_PROFILE_ID,
                                       validated ? WALTER_MODEM_TLS_VALIDATION_CA
                                                 : WALTER_MODEM_TLS_VALIDATION_NONE,
                                       WALTER_MODEM_TLS_VERSION_12, ca_slot)) {
        ESP_LOGI(TAG, "net_tls_configure(): tlsConfigProfile() failed (slot=%u validated=%d)",
                 (unsigned) ca_slot, (int) validated);
        return false;
    }
    ESP_LOGI(TAG, "TLS profile %d reconfigured: slot=%u %s", PAGER_TLS_PROFILE_ID, (unsigned) ca_slot,
             validated ? "VALIDATION_CA" : "VALIDATION_NONE");
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
        // v0.2 §5 trigger 2 (motion): a second, independent wake pin needs
        // ext1, not a second ext0 -- the ESP32-S3 (like every ESP32 variant)
        // has exactly one ext0 source (a single fixed RTC GPIO, already
        // spoken for by the button) but ext1 takes a bitmask of any number
        // of RTC GPIOs sharing one level mode. IO2 (LIS3DH INT1) is
        // configured push-pull active-high (accel.c), so ANY_HIGH is the
        // right mode for a one-pin mask; it does not need to agree with
        // ext0's own (unrelated) active-low button polarity -- the two wake
        // sources are independent and can coexist armed simultaneously.
        // Only armed once accel.c has confirmed the chip actually answers
        // WHO_AM_I (net_enable_accel_wake()) -- an unwired/floating IO2
        // armed as ANY_HIGH would wake the ESP32 on every light-sleep cycle
        // for nothing, which is expected to be the common case on the
        // owner's bench unit (accel.c's own module comment).
        if (s_accel_wake_enabled) {
            esp_sleep_enable_ext1_wakeup(1ULL << PAGER_PIN_LIS3DH_INT1, ESP_EXT1_WAKEUP_ANY_HIGH);
        }
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

static size_t s_nettest_pad_bytes = 0;

// Debug console `at <command>`: sends one raw AT command and relies on the
// WalterModem debug trace to show the reply. Debug build only.
extern "C" bool net_debug_at(const char *cmd)
{
    return WalterModem::sendCmd(cmd);
}

extern "C" bool net_check_tcp_sized(const char *host, uint16_t port, size_t bytes)
{
    s_nettest_pad_bytes = bytes;
    bool ok = net_check_tcp(host, port, false, false);
    s_nettest_pad_bytes = 0;
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
    // Optional sized payload (s_nettest_pad_bytes, set by net_check_tcp_sized()):
    // a valid HTTP/1.0 GET padded with a dummy header to the requested size,
    // after which we wait and let the AT trace show whether the modem rings
    // with a reply (+SQNSRING). Separates "the carrier/path mishandles larger
    // uplink segments" from "TLS specifically fails": last night's plain test
    // only ever sent 13 bytes, a TLS ClientHello is ~215.
    static char big[1400];
    const char *test_payload = "pager nettest";
    size_t test_len = strlen(test_payload);
    if (s_nettest_pad_bytes > 0) {
        int n = snprintf(big, sizeof(big), "GET / HTTP/1.0\r\nHost: %s\r\nX-Pad: ", host);
        size_t want = s_nettest_pad_bytes > sizeof(big) - 8 ? sizeof(big) - 8 : s_nettest_pad_bytes;
        while ((size_t) n + 4 < want) {
            big[n++] = 'a';
        }
        memcpy(big + n, "\r\n\r\n", 4);
        n += 4;
        test_payload = big;
        test_len = (size_t) n;
    }
    bool sent = WalterModem::socketSend(PAGER_TCP_TEST_SOCKET_ID, (uint8_t *) test_payload,
                                        (uint16_t) test_len);
    ESP_LOGI(TAG, "nettest: socketSend (%u bytes): %s", (unsigned) test_len, sent ? "OK" : "FAILED");
    if (s_nettest_pad_bytes > 0) {
        ESP_LOGI(TAG, "nettest: waiting 12 s for a reply; look for '+SQNSRING: %d,<bytes>' in the trace",
                 PAGER_TCP_TEST_SOCKET_ID);
        vTaskDelay(pdMS_TO_TICKS(12000));
    }
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

// ---------------------------------------------------------------------------
// v0.2 §5 (location, loc.c). See net.h's own doc comments for the contract
// each of these follows; this section is the only place that turns loc.c's
// small facade calls into real WalterModem GNSS/opstate API use.
// ---------------------------------------------------------------------------

extern "C" bool net_gnss_config(void)
{
    // Power effect: one AT command, no RRC, does not power the GNSS receiver
    // by itself (gnssPerformAction() below does that).
    if (!WalterModem::gnssConfig(WALTER_MODEM_GNSS_SENS_MODE_HIGH,
                                 WALTER_MODEM_GNSS_ACQ_MODE_COLD_WARM_START,
                                 WALTER_MODEM_GNSS_LOC_MODE_ON_DEVICE_LOCATION)) {
        ESP_LOGI(TAG, "gnssConfig() failed");
        return false;
    }
    return true;
}

extern "C" bool net_gnss_assistance_due(int32_t *out_seconds_to_update)
{
    WalterModemRsp rsp = {};
    if (!WalterModem::gnssGetAssistanceStatus(&rsp)) {
        ESP_LOGI(TAG, "gnssGetAssistanceStatus() failed");
        return false;
    }
    const WMGNSSAssistance &a =
        rsp.data.gnssAssistance[WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS];
    if (out_seconds_to_update) {
        *out_seconds_to_update = a.timeToUpdate;
    }
    ESP_LOGI(TAG, "gnss real-time ephemeris: available=%d timeToUpdate=%lds timeToExpire=%lds",
             (int) a.available, (long) a.timeToUpdate, (long) a.timeToExpire);
    return true;
}

extern "C" bool net_gnss_update_assistance(void)
{
    int64_t t0_us = esp_timer_get_time();
    bool ok = WalterModem::gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS);
    int64_t elapsed_ms = (esp_timer_get_time() - t0_us) / 1000;
    // V02_DESIGN.md §5: "log the bytes it costs" -- the vendor API reports
    // neither bytes nor a progress callback, so elapsed time is the nearest
    // stand-in available; UNVERIFIED what real byte cost that corresponds to.
    ESP_LOGI(TAG, "gnssUpdateAssistance(REALTIME_EPHEMERIS): %s, %lld ms",
             ok ? "ok" : "failed", (long long) elapsed_ms);
    return ok;
}

extern "C" bool net_gnss_start_fix(void)
{
    if (!WalterModem::gnssPerformAction(WALTER_MODEM_GNSS_ACTION_GET_SINGLE_FIX)) {
        ESP_LOGI(TAG, "gnssPerformAction(GET_SINGLE_FIX) refused synchronously");
        return false;
    }
    ESP_LOGI(TAG, "gnss single-fix action accepted; waiting for a GNSS event");
    return true;
}

extern "C" void net_gnss_cancel(void)
{
    if (!WalterModem::gnssPerformAction(WALTER_MODEM_GNSS_ACTION_CANCEL)) {
        ESP_LOGI(TAG, "gnssPerformAction(CANCEL) failed (best-effort)");
    }
}

extern "C" bool net_gnss_poll_event(net_gnss_event_t *out)
{
    if (!s_gnss_event_pending) {
        return false;
    }
    if (out) {
        *out = s_gnss_event;
    }
    s_gnss_event_pending = false;
    return true;
}

extern "C" bool net_radio_off(void)
{
    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGI(TAG, "setOpState(NO_RF) failed (loc route 2)");
        return false;
    }
    return true;
}

extern "C" bool net_radio_on(void)
{
    if (!WalterModem::setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        ESP_LOGI(TAG, "setOpState(FULL) failed (loc route 2 restore)");
        return false;
    }
    return true;
}

extern "C" bool net_is_attached(void)
{
    // Power effect: one AT round trip ("AT+CEREG?"), no RRC of its own --
    // same cost class as net_check(). WalterModem::getNetworkRegState()
    // genuinely blocks on this command (confirmed by reading the vendor
    // source, not an accessor of already-tracked state), so loc.c calls this
    // at most once per loc_service() iteration while polling for re-attach.
    WalterModemNetworkRegState st = WalterModem::getNetworkRegState();
    return st == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
           st == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING;
}

extern "C" void net_set_cell_change_cb(void (*cb)(const char *cell_key))
{
    s_cell_change_cb = cb;
}

extern "C" void net_enable_accel_wake(void)
{
    s_accel_wake_enabled = true;
}

// ---------------------------------------------------------------------------
// v0.2 §4.4 (CA trust, cafetch.c). See net.h's own doc comments for the
// contract each of these follows.
// ---------------------------------------------------------------------------

extern "C" bool net_ca_fetch_open(const char *host, uint16_t port)
{
    // v0.2 bug fix #4 (§2.4)/BRINGUP_NOTES.md's rule applies to this profile
    // too: never leave PAGER_TLS_CA_SLOT empty before naming it in a TLS
    // profile, even one that never validates against it.
    ensure_ca_slot_populated();

    if (!WalterModem::tlsConfigProfile(PAGER_CA_FETCH_TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_NONE,
                                       WALTER_MODEM_TLS_VERSION_12, PAGER_TLS_CA_SLOT)) {
        ESP_LOGI(TAG, "cafetch: tlsConfigProfile(profile %d) failed", PAGER_CA_FETCH_TLS_PROFILE_ID);
        return false;
    }
    if (!WalterModem::socketConfig(PAGER_CA_FETCH_SOCKET_ID)) {
        ESP_LOGI(TAG, "cafetch: socketConfig() failed");
        return false;
    }
    if (!WalterModem::socketConfigSecure(PAGER_CA_FETCH_SOCKET_ID, true, PAGER_CA_FETCH_TLS_PROFILE_ID)) {
        ESP_LOGI(TAG, "cafetch: socketConfigSecure() failed");
        return false;
    }

    s_ca_fetch_ring_pending = false;
    s_ca_fetch_closed = false;

    WalterModemRsp rsp = {};
    if (!WalterModem::socketDial(PAGER_CA_FETCH_SOCKET_ID, WALTER_MODEM_SOCKET_PROTO_TCP, port, host, 0,
                                 WALTER_MODEM_ACCEPT_ANY_REMOTE_DISABLED, &rsp)) {
        ESP_LOGI(TAG, "cafetch: socketDial() failed (result=%s)", walter_state_name(rsp.result));
        return false;
    }
    ESP_LOGI(TAG, "cafetch: socket %d dialed to %s:%u (TLS profile %d, VALIDATION_NONE)",
             PAGER_CA_FETCH_SOCKET_ID, host, (unsigned) port, PAGER_CA_FETCH_TLS_PROFILE_ID);
    return true;
}

extern "C" bool net_ca_fetch_send(const uint8_t *buf, uint16_t len)
{
    // L6-style cast (net_publish_raw() above): the vendor's socketSend()
    // takes uint8_t*, not const, but never mutates the caller's buffer (it
    // only reads it onto the wire after the modem's own framing).
    return WalterModem::socketSend(PAGER_CA_FETCH_SOCKET_ID, (uint8_t *) (uintptr_t) buf, len);
}

extern "C" bool net_ca_fetch_poll(uint8_t *buf, size_t cap, uint16_t *out_len, bool *out_closed)
{
    if (out_closed) {
        *out_closed = false;
    }
    if (out_len) {
        *out_len = 0;
    }

    // Data first, close second. Found on hardware: with `Connection: close`
    // the reply's +SQNSRING and the peer's +SQNSH arrive in the same few
    // milliseconds, and reporting the close first threw the whole response
    // away unread ("parse failed, 0 body bytes"). So: while a ring is pending
    // OR the peer has closed, try to read; only report the close once a read
    // comes back empty. The modem keeps received bytes readable after +SQNSH
    // (UNVERIFIED beyond the single-segment responses tested).
    if (s_ca_fetch_ring_pending || s_ca_fetch_closed) {
        WalterModemRsp rsp = {};
        uint16_t got = 0;
        if (WalterModem::socketReceive(PAGER_CA_FETCH_SOCKET_ID, buf, cap, &rsp)) {
            got = rsp.data.socketResponse.bytesReceived;
        } else if (!s_ca_fetch_closed) {
            ESP_LOGI(TAG, "cafetch: socketReceive() failed");
        }
        if (got > 0) {
            // More may be waiting than one read returns (<=1500 B per
            // AT+SQNSRECV), and the modem does not always ring again: keep
            // the ring armed until a read comes back empty.
            s_ca_fetch_ring_pending = true;
            if (out_len) {
                *out_len = got;
            }
            return true;
        }
        s_ca_fetch_ring_pending = false;
        if (s_ca_fetch_closed) {
            s_ca_fetch_closed = false;
            if (out_closed) {
                *out_closed = true;
            }
        }
        return true;
    }
    return false;
}

extern "C" void net_ca_fetch_close(void)
{
    WalterModem::socketClose(PAGER_CA_FETCH_SOCKET_ID); // best-effort, power effect: one AT command
    s_ca_fetch_ring_pending = false;
    s_ca_fetch_closed = false;
}

// ---------------------------------------------------------------------------
// v0.2 §6 (device-direct SMS, sms.c). See net.h's own doc comments for the
// contract each of these follows; the vendor patch itself (PATCHES.md 1.4)
// documents the AT command sequence.
// ---------------------------------------------------------------------------

extern "C" bool net_sms_config(net_sms_config_result_t *out)
{
    if (out) {
        out->used_ira = false;
        out->storage_used = -1;
        out->storage_total = -1;
    }
    bool usedIra = false;
    WalterModemRsp rsp = {};
    if (!WalterModem::smsConfig(&usedIra, &rsp)) {
        ESP_LOGI(TAG, "smsConfig() failed - SMS unavailable this boot (V02_DESIGN.md §0: fail open)");
        return false;
    }
    ESP_LOGI(TAG, "smsConfig() OK, resting charset=%s", usedIra ? "IRA" : "GSM (fallback)");
    if (out) {
        out->used_ira = usedIra;
        // rsp holds the LAST sub-command's own data (AT+CPMS=, smsConfig()'s
        // final step) -- see WalterModem::smsConfig()'s own doc comment.
        if (rsp.type == WALTER_MODEM_RSP_DATA_TYPE_SMS_STORAGE) {
            out->storage_used = rsp.data.smsStorage.usedr;
            out->storage_total = rsp.data.smsStorage.totalr;
        }
    }
    return true;
}

extern "C" bool net_sms_send(const char *number, const char *text, bool use_ucs2)
{
    WalterModemRsp rsp = {};
    if (!WalterModem::smsSend(number, text, use_ucs2, &rsp)) {
        ESP_LOGI(TAG, "smsSend() failed (result=%s)", walter_state_name(rsp.result));
        return false;
    }
    return true;
}

extern "C" bool net_sms_read(int index, net_sms_read_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    WalterModemRsp rsp = {};
    if (!WalterModem::smsRead(index, &rsp)) {
        ESP_LOGI(TAG, "smsRead(%d) failed (result=%s)", index, walter_state_name(rsp.result));
        return false;
    }
    if (out) {
        out->valid = rsp.data.smsRead.valid;
        out->dcs = rsp.data.smsRead.dcs;
        strncpy(out->sender, rsp.data.smsRead.sender, sizeof(out->sender) - 1);
        strncpy(out->timestamp, rsp.data.smsRead.timestamp, sizeof(out->timestamp) - 1);
        uint16_t n = rsp.data.smsRead.bodyLen;
        if (n > sizeof(out->body) - 1) {
            n = sizeof(out->body) - 1;
        }
        memcpy(out->body, rsp.data.smsRead.body, n);
        out->body[n] = '\0';
        out->body_len = n;
    }
    return true;
}

extern "C" bool net_sms_delete(int index)
{
    WalterModemRsp rsp = {};
    if (!WalterModem::smsDelete(index, &rsp)) {
        ESP_LOGI(TAG, "smsDelete(%d) failed (result=%s)", index, walter_state_name(rsp.result));
        return false;
    }
    return true;
}

extern "C" bool net_sms_poll_event(net_sms_event_t *out)
{
    if (!s_sms_event_pending) {
        return false;
    }
    if (out) {
        *out = s_sms_event;
    }
    s_sms_event_pending = false;
    return true;
}

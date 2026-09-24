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
#include "net_xport.h"
#include "net_internal.h"
#include "pins.h"
#include "ident.h"
#include "carrier.h"
#include "watchdog.h"
#include "placeholder_ca.h"
#include "net_connect_guard.h"
#include "net_probe_guard.h"
#include "publish_quiet.h"
#include "wifi_sta.h"

#include "WalterModem.h"
#include "WalterDefines.h" // walter_modem_pager_counters() (PATCHES.md 1.12)

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

// docs/WIFI_TASKS.md W4: PAGER_MQTT_KEEPALIVE_S, PAGER_MQTT_PING_S,
// PAGER_MAX_PAYLOAD, PAGER_BOOT_MAX_PAYLOAD and PAGER_BOOT_TOPIC_PREFIX moved
// to xport_lte.cpp with the MQTT session code that is their only use.

static constexpr int PAGER_ATTACH_POLL_CAP_S = 300; // F1: single-attempt cap

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

// Registers one callback that both transports call (docs/WIFI_DESIGN.md §1):
// a single, transport-independent slot rather than per-transport state, so a
// caller's registration survives a future net_xport_switch(). Not `static`:
// xport_lte.cpp's pager_mqtt_event_handler() (net_internal.h) invokes it
// directly, exactly as it did when both lived in this file.
void (*s_msg_cb)(const char *, const char *, uint16_t) = nullptr;

// ---------------------------------------------------------------------------
// The transport seam (docs/WIFI_DESIGN.md §1, docs/WIFI_TASKS.md W4).
// s_xport_ops is the only thing every dispatcher (below, past net_tls_configure())
// touches; s_active_xport is only ever written here (net_init(), today's sole
// writer -- W5's net_xport_switch() becomes the only other one). Every net.h
// entry point in docs/WIFI_DESIGN.md §1's "dispatched through the seam" list
// is one of those thin wrappers; the real logic is the unmodified code moved
// to xport_lte.cpp (see that file's own module comment).
// ---------------------------------------------------------------------------
// Defaults to the LTE implementation at load, NOT in net_init(): modes_boot()
// paints the boot screen (draw_status_bar() -> net_get_mqtt_status()) before
// net_init() runs, and a null vtable here boot-looped the pager on 23 Sep
// (LoadProhibited at net.cpp's status dispatcher, phaseU-inject.log).
static const net_xport_ops_t *s_xport_ops = xport_lte_ops();
static net_xport_t s_active_xport = NET_XPORT_LTE;
// v0.2 §9.4's three suppressions (coverage duty cycle, location route 2, a CA
// -apply trial), moved here from modes.c's net_service_session() call site --
// see net_set_lte_suppressed()'s own doc comment in net.h for why.
static bool s_lte_suppressed = false;

// docs/WIFI_TASKS.md W4: moved to xport_lte.cpp with the rest of the MQTT
// session state (net_internal.h re-declares what net.cpp still needs).
// s_down_topic is the one piece of LTE bringup state configure_session()
// (below) and net_bootstrap_connect() write and xport_lte.cpp's MQTT event
// handler/net_service_session() read -- not `static` for the same reason.
char s_down_topic[48];
static char s_granted_edrx[16] = { 0 };

// DEVICE_PLAN.md §3.3: the hash of the CA last written to modem NVRAM slot
// PAGER_TLS_CA_SLOT, so repeated net_init() calls within one power session
// (F4 recovery) skip the NVRAM write when ident's ca_hash has not changed.
// Deliberately NOT in RTC/NVS: it only needs to survive net_recover_modem(),
// not a real reboot — a real reboot always rewriting once is harmless and
// simpler than persisting this across resets.
static bool s_ca_written = false;
static uint8_t s_ca_written_hash[IDENT_CA_HASH_LEN];

// docs/WIFI_TASKS.md W4: s_mqtt_rx_buf moved to xport_lte.cpp (its only
// reader/writer, pager_mqtt_event_handler's MESSAGE case, moved with it).

static int64_t s_clock_epoch = 0;    // 0 = no network time yet (§3.5)
static int64_t s_clock_epoch_us = 0; // esp_timer_get_time() at the moment s_clock_epoch was read

static bool s_wake_sources_armed = false;

// S1 (docs/SLEEP_URC_DESIGN.md §5(1)): true once WalterModem::begin() has
// succeeded and stays false only across a net_recover_modem() reset --
// net_urc_probe()'s "modem is not begun / is in a reset" guard. Written only
// by net_bringup()/net_bootstrap_attach()/net_recover_modem(), all on the
// caller's task (single-threaded with respect to net_urc_probe()'s own
// caller, modes.c's main loop).
static bool s_modem_begun = false;

// S1 (docs/SLEEP_URC_DESIGN.md §3(a)/§5): the URC drain probe's single-slot
// in-flight guard (net_probe_guard.h) -- declared here, near the rest of
// this file's state, and initialised alongside s_connect_guard/
// s_publish_quiet in net_init()/net_recover_modem() (see those call sites).
static net_probe_guard_t s_probe_guard;

// S7b post-mortem (docs/SLEEP_URC_DESIGN.md §9): the two "the probe was not
// even attempted this wake" counts. Deliberately NOT in net_probe_guard_t:
// they are not part of the guard's decision logic (nothing reads them back),
// they are report fields, and keeping them here leaves the host-tested pure
// module untouched. Written only by net_urc_probe(), on modes.c's task.
static uint32_t s_probe_skip_busy = 0;
static uint32_t s_probe_skip_down = 0;

// A1 (docs/DEVICE_NEXT_TASKS.md): count of light-sleep returns woken by
// ext1 (the LIS3DH motion pin), since boot. net_get_ext1_wakes().
static uint32_t s_ext1_wakes = 0;

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

// Serving-cell cache (PROTOCOL.md §13.2 `cell`, this task) -- net_get_cell_info()'s
// own doc comment (net.h) explains the staleness policy: fetched once via
// AT+SQNMONI on the first call, then again only after a genuine cell change
// (s_cell_info_stale set alongside s_last_cell_key above, same "if changed"
// branch). Starts stale (true) so the very first request-driven fetch always
// runs, never a guess.
static net_cell_info_t s_cell_cache = {};
static bool s_cell_info_stale = true;

// v0.2 §5: arms IO2 (LIS3DH INT1) as an ext1 light-sleep wake source, only
// once accel.c has confirmed the chip is actually present (see
// net_enable_accel_wake()'s own doc comment in net.h).
static bool s_accel_wake_enabled = false;

// docs/WIFI_TASKS.md W4: classify_mqtt_rc() and pager_mqtt_event_handler()
// moved to xport_lte.cpp, unmodified, with the rest of the MQTT session code
// (docs/WIFI_DESIGN.md §1). pager_mqtt_event_handler() is still registered
// from net_bringup()/net_bootstrap_attach() below via
// WalterModem::setMQTTEventHandler() -- net_internal.h declares it so those
// two call sites keep compiling unchanged.
//
// ---------------------------------------------------------------------------
// Event handlers below run on WalterModem's _eventProcessingTask
// (WalterModem.cpp:1595-1626), NOT the RX task/ISR. Calling modem APIs from
// here is the vendor's own pattern (examples/mqtts). Keep these short (L4).
// ---------------------------------------------------------------------------

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

// note_registration() is declared in net_internal.h (defined with the
// coverage-tracking state below; not `static` since xport_lte.cpp's
// net_session_up() also calls it -- docs/WIFI_TASKS.md W4).

static void pager_network_event_handler(WMNetworkEventType event, const WMNetworkEventData *data, void *args)
{
    (void) args;

    if (event == WALTER_MODEM_NETWORK_EVENT_REG_STATE_CHANGE) {
        ESP_LOGI(TAG, "network registration state -> %d", (int) data->cereg.state);
        note_registration(data->cereg.state == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
                          data->cereg.state == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
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
                // Just a flag write, not a modem call -- legal from an event
                // callback (same class as s_disconnect_edge/s_gnss_event_pending
                // elsewhere in this file). The actual AT+SQNMONI refresh happens
                // in net_get_cell_info(), from loc.c's own task.
                s_cell_info_stale = true;
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
// is *empty* -- GOTCHAS.md only confirmed that a profile which does
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
                      "empty named slot stays UNVERIFIED (GOTCHAS.md)",
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

// ---------------------------------------------------------------------------
// Coverage loss and recovery.
//
// Found on hardware (a walk in and out of a building killed the pager until a
// power cycle): net_init() used to do everything in one pass and give up if
// the attach did not happen, and nothing ever ran it again. Worse, the modem
// health check treated "not registered" as "modem unresponsive" and hard-reset
// the modem, which wipes its MQTT and TLS configuration; if there was still no
// coverage the re-init stopped before restoring them, so every later connect
// failed with +CME ERROR for ever, even with full signal.
//
// Now: bringing the radio up and configuring the session are separate. Losing
// coverage is normal and resets nothing: the modem re-registers by itself
// (COPS=0) and net_session_up() (re)configures whatever is missing the next
// time it runs while registered. A modem reset only clears a flag.
// ---------------------------------------------------------------------------
// Not `static` (net_internal.h declares both): docs/WIFI_TASKS.md W4 moved
// net_session_up() to xport_lte.cpp, and it still reads s_registered and
// calls note_registration() exactly as it did in this file.
volatile bool s_registered = false;        // tracked from +CEREG URCs and polls
static volatile bool s_reg_regained_edge = false; // not registered -> registered
static volatile int64_t s_unregistered_since_us = 0;
bool s_session_configured = false;         // TLS profile + mqttConfig are in the modem

void note_registration(bool registered)
{
    if (registered && !s_registered) {
        s_reg_regained_edge = true;
        s_unregistered_since_us = 0;
    } else if (!registered && (s_registered || s_unregistered_since_us == 0)) {
        s_unregistered_since_us = esp_timer_get_time();
    }
    s_registered = registered;
}

bool configure_session(void); // not `static`: net_internal.h re-declares this for xport_lte.cpp

static bool net_bringup(int attach_wait_s)
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
    s_modem_begun = true; // S1: net_urc_probe()'s "not begun" guard clears here

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
    // The wait is a convenience (it lets boot go straight to a connected
    // session), not a condition: on a timeout the radio stays up and
    // searching, and the session is configured later, from net_session_up().
    bool attached = false;
    for (int waited_s = 0; waited_s < attach_wait_s; waited_s++) {
        watchdog_feed(); // up to 300 s of legitimate waiting at boot
        WalterModemNetworkRegState st = WalterModem::getNetworkRegState();
        if (st == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
            st == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) {
            attached = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    note_registration(attached);
    if (!attached) {
        ESP_LOGI(TAG, "no network after %d s; the radio stays on and the session will be set up "
                      "when coverage appears",
                 attach_wait_s);
        return true;
    }
    ESP_LOGI(TAG, "network attached");
    return configure_session();
}

extern "C" bool net_init(void)
{
    // docs/WIFI_TASKS.md W4: wire the transport seam to its one implementation.
    // LTE always, in this task -- W5's net_xport_switch() is the only future
    // writer of s_active_xport (already NET_XPORT_LTE by its static initializer).
    s_xport_ops = xport_lte_ops();
    s_session_configured = false;
    net_connect_guard_init(&s_connect_guard); // v0.2 M1/M3: fresh boot, nothing outstanding
    publish_quiet_gate_init(&s_publish_quiet); // 23 Sep fix: fresh boot, nothing in flight
    net_probe_guard_init(&s_probe_guard); // S1: fresh boot, nothing outstanding
    return net_bringup(PAGER_ATTACH_POLL_CAP_S);
}

// Clock, CA, TLS profile and MQTT client configuration: everything a modem
// reset wipes. Needs the network only for the clock. Power effect: a handful of
// AT commands, one NVRAM write if the CA changed.
// Not `static` (net_internal.h's own forward declaration): docs/WIFI_TASKS.md
// W4 moved net_session_up() to xport_lte.cpp, and it still calls this.
bool configure_session(void)
{

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
    for (int attempt = 0; s_clock_epoch == 0 && attempt < 10; attempt++) {
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

    s_session_configured = true;
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
    s_modem_begun = true; // S1: net_urc_probe()'s "not begun" guard clears here too

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
    // GOTCHAS.md plaintext-fallback shape). Power effect: one AT
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
    return s_xport_ops->up();
}

extern "C" void net_session_down(void)
{
    s_xport_ops->down();
}

extern "C" bool net_publish(const char *topic, char *buf, uint16_t len, uint8_t qos)
{
    return s_xport_ops->publish(topic, buf, len, qos);
}

extern "C" bool net_publish_raw(const char *topic, uint8_t *buf, uint16_t len, uint8_t qos)
{
    return s_xport_ops->publish_raw(topic, buf, len, qos);
}

extern "C" uint32_t net_publish_quiet_wait_ms(uint32_t max_wait_ms)
{
    return s_xport_ops->publish_quiet_wait_ms(max_wait_ms);
}

extern "C" net_xport_t net_xport_active(void)
{
    return s_active_xport;
}

extern "C" void net_set_lte_suppressed(bool suppressed)
{
    s_lte_suppressed = suppressed;
}

// docs/WIFI_DESIGN.md §2/§1, docs/WIFI_TASKS.md W5: the only writer of
// s_active_xport besides net_init()'s own static initializer. Tears the old
// transport's session down, waits (bounded) for its own DISCONNECTED, then
// brings the new one up -- "at most one MQTT client object exists at a
// time" is the hard invariant this function exists to hold (design §2/§8:
// EMQX kicks the older session on a client-id collision otherwise, which
// looks exactly like "pages stopped arriving").
//
// Deviation from docs/WIFI_TASKS.md W5's literal wording ("asserted"): a
// real assert()/abort() here would let a console typo or a slow broker
// crash a device that is otherwise fine on its old transport. This logs at
// ERROR and proceeds with the switch instead -- the new transport's own
// .up() starts a fresh client regardless, so a slow-to-disconnect old one
// costs at most an EMQX-side kick (a delivery gap on the transport being
// abandoned anyway), never a crash. Flagged for the architect.
#define NET_XPORT_SWITCH_WAIT_US ((int64_t) 2 * 1000000)

static const char *xport_name(net_xport_t x)
{
    return (x == NET_XPORT_WIFI) ? "WIFI" : "LTE";
}

extern "C" void net_xport_switch(net_xport_t to)
{
    if (to == s_active_xport) {
        return;
    }
    net_xport_t from = s_active_xport;
    const net_xport_ops_t *old_ops = s_xport_ops;

    old_ops->down();

    net_mqtt_status_t st = {};
    int64_t deadline = esp_timer_get_time() + NET_XPORT_SWITCH_WAIT_US;
    do {
        old_ops->status(&st);
        if (!st.mqtt_connected) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (esp_timer_get_time() < deadline);
    if (st.mqtt_connected) {
        ESP_LOGE(TAG, "xport switch %s -> %s: old transport still reports connected after %llds, "
                      "switching anyway",
                 xport_name(from), xport_name(to), (long long) (NET_XPORT_SWITCH_WAIT_US / 1000000));
    }

    s_active_xport = to;
    s_xport_ops = (to == NET_XPORT_WIFI) ? xport_wifi_ops() : xport_lte_ops();
    // Power effect: logged once per transport change (docs/WIFI_TASKS.md
    // W5's own verify step) -- LTE keeps the modem's MQTT session (a TLS
    // handshake, ~5kB); WIFI brings up esp-mqtt over the STA (a second,
    // independent TLS handshake, no modem involvement at all).
    ESP_LOGI(TAG, "xport: %s -> %s", xport_name(from), xport_name(to));

    if (!s_xport_ops->up()) {
        ESP_LOGI(TAG, "xport switch %s -> %s: new transport's up() failed (see its own log line)",
                 xport_name(from), xport_name(to));
    }
}

extern "C" void net_set_msg_cb(void (*cb)(const char *topic, const char *body, uint16_t len))
{
    s_msg_cb = cb;
}

// docs/WIFI_TASKS.md W5: xport_wifi.c's own trampoline to s_msg_cb -- see
// net_xport.h's doc comment on this function for why it exists instead of a
// direct net_internal.h include from that file.
extern "C" void net_dispatch_msg(const char *topic, const char *body, uint16_t len)
{
    if (s_msg_cb) {
        s_msg_cb(topic, body, len);
    }
}

extern "C" void net_sleep(uint32_t ms)
{
    // docs/WIFI_DESIGN.md §3/§9, docs/WIFI_TASKS.md W5: transport-aware.
    // ESP-IDF documents that a WiFi association is not maintained across a
    // manual esp_light_sleep_start() (sleep_modes.rst:49-51); the supported
    // way to keep one is Modem-sleep + *automatic* light sleep
    // (CONFIG_PM_ENABLE, W9), which phase 1 does not enable. So on WiFi this
    // is a plain, bounded vTaskDelay(): CPU/USB/log stay up, current stays
    // at the ~38-40 mA WiFi-associated floor (docs/WIFI_DESIGN.md §0's
    // table, row (a)) for `ms` -- known and accepted for phase 1 (USB/bench
    // power only, §3's own framing); battery viability is W9/W10's job, not
    // this function's. wifi_sta_set_ps_sleep()/_active() around the delay
    // is the one power lever phase 1 has without PM_ENABLE: WIFI_PS_MAX_MODEM
    // (listen_interval 10) for the duration of the delay, back to
    // WIFI_PS_MIN_MODEM once it returns.
    if (s_active_xport == NET_XPORT_WIFI) {
        wifi_sta_set_ps_sleep();
        vTaskDelay(pdMS_TO_TICKS(ms));
        wifi_sta_set_ps_active();
        return;
    }

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
    // v0.2 §5 trigger 2 (motion): a second, independent wake pin needs ext1,
    // not a second ext0 -- the ESP32-S3 (like every ESP32 variant) has
    // exactly one ext0 source (a single fixed RTC GPIO, already spoken for
    // by the button) but ext1 takes a bitmask of any number of RTC GPIOs
    // sharing one level mode. IO2 (LIS3DH INT1) is configured push-pull
    // active-high (accel.c), so ANY_HIGH is the right mode for a one-pin
    // mask; it does not need to agree with ext0's own (unrelated) active-low
    // button polarity -- the two wake sources are independent and can
    // coexist armed simultaneously.
    //
    // A1: unlike ext0/button above, this is evaluated fresh on *every*
    // net_sleep() call rather than latched once -- accel.c toggles
    // s_accel_wake_enabled off for a refractory window after every edge it
    // reports (net_set_accel_wake()), so a wake storm while the pager is
    // being carried does not end light sleep ~10x/s
    // (docs/DEVICE_NEXT_TASKS.md A1). Still never armed at all if the chip
    // never answered WHO_AM_I (accel.c's own module comment: an
    // unwired/floating IO2 armed as ANY_HIGH would wake the ESP32 on every
    // light-sleep cycle for nothing).
    if (s_accel_wake_enabled) {
        esp_sleep_enable_ext1_wakeup(1ULL << PAGER_PIN_LIS3DH_INT1, ESP_EXT1_WAKEUP_ANY_HIGH);
    } else {
        // Harmless (ESP_ERR_INVALID_STATE, ignored) if ext1 was never armed
        // in the first place -- e.g. the chip is absent, or this is the
        // very first net_sleep() call before accel_init() has run yet.
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
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
    // docs/RCA_SLEEP_URC.md §2: the three lines above do NOT actually hold RTS
    // high across the sleep. CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y
    // (firmware/sdkconfig:825) makes IDF run esp_sleep_config_gpio_isolate()
    // + esp_sleep_enable_gpio_switch(true) at system init
    // (esp-idf/components/esp_hw_support/sleep_gpio.c:187-199): every valid
    // GPIO gets SLP_OE=0 / SLP_IE=0 / no pull in its IO_MUX *sleep* fields and
    // its SLP_SEL bit set, so the pad hardware switches to "output driver off,
    // floating" for the whole of esp_light_sleep_start() and back on wake. No
    // software runs at that moment; nothing above can prevent it. The modem's
    // CTS input is therefore driven by a board pull we do not control for
    // ~2000 ms of every 2200 ms cycle, which turns PROTOCOL.md §8.3/M5's "does
    // the Sequans queue or drop while CTS is deasserted" into "is the Sequans
    // even seeing CTS deasserted" -- and a modem that reads CTS as asserted
    // transmits its held +SQNSMQTTONMESSAGE into a UART whose clock is gated,
    // i.e. straight onto the floor.
    //
    // Excluding RTS from the automatic switch keeps the awake configuration
    // (plain GPIO output, level 1) live through the sleep, which is what the
    // vendor's own WalterModem::sleep() (WalterModem.cpp:5054-5097) always
    // believed it was doing. One IO_MUX register write per sleep; no power
    // cost (the pad is driven high either way, into a CMOS input).
    //
    // What proves it: pages must stop showing the 35 s / 136 s / 182 s tail
    // (GOTCHAS.md "Pages never arrive while the pager sleeps";
    // build/bench-logs/phase1-savedreport.log) and instead land within one
    // wake cycle. The counter to add for a quantitative answer is
    // "bytes read from the modem UART in the first 50 ms after each wake"
    // in the sleeptest report: near-zero per wake today, a burst on the wake
    // after a page once this holds.
    gpio_sleep_sel_dis((gpio_num_t) CONFIG_WALTER_MODEM_PIN_RTS);

    esp_light_sleep_start();

    // A1: count ext1 (motion) wakes, before anything below can touch the
    // wakeup-cause register -- net_get_ext1_wakes() is A2's `acceltest`
    // print and A4's bench measurement of this task's whole justification
    // (refr 0 vs. refr 20, expect roughly two orders of magnitude fewer).
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        s_ext1_wakes++;
    }

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
    // "Responsive" only. Being out of coverage is not a modem fault and must
    // not lead to a modem reset (see the block comment above net_bringup()).
    if (!WalterModem::checkComm()) {
        return false;
    }
    note_registration(net_is_attached());
    return true;
}

// ---------------------------------------------------------------------------
// S1 (docs/SLEEP_URC_DESIGN.md §3(a)/§5): per-wake URC drain probe.
// ---------------------------------------------------------------------------

// Per-attempt budget for the probe's single bare "AT" (PAGER PATCH 1.14,
// WalterModem::checkComm()'s two new pass-through parameters). See the call
// site in net_urc_probe() for the measurement that set these.
#define PAGER_URC_PROBE_ATTEMPTS 1
// S18 (docs/SLEEP_URC_DESIGN.md §10), raised from 2000: the host now stays
// awake with RTS asserted until this probe is ANSWERED (modes.c's
// wait_for_probe_answer(), bounded by PAGER_PROBE_WAIT_MS), so a budget
// SHORTER than that wait guarantees the answer is orphaned -- the library
// gives up, clears _curCmd, and the modem's "OK" arrives with no command to
// pair it against (phaseAK: probe_timedout=10 of 18, rsp_no_cmd=3). The
// budget must therefore exceed the host's own wait bound, not undercut it.
// Still ONE attempt: the flush this probe exists to trigger happens when the
// modem accepts the command, so a retry buys nothing (§8.4).
// NOTE, and it is load-bearing: this budget is in FreeRTOS ticks, which do
// NOT advance across a manual esp_light_sleep_start() (ESP-IDF adjusts only
// esp_timer, sleep_modes.c:1263; there is no vTaskStepTick/xTaskCatchUpTicks
// on that path). While the pager light-sleeps at a ~4% duty a "6 s" budget is
// ~150 s of wall clock. It is a real 6 s only while the host stays awake --
// which, with the wait above, is exactly the window that matters.
#define PAGER_URC_PROBE_TIMEOUT_MS 6000u

// The guard's decision logic (single-slot in-flight, stuck-after-3-wakes, no
// double queue) is pure C, unit tested on the host without a device --
// firmware/host/test_net_probe_guard.c -- same split as net_connect_guard.h
// (s_probe_guard itself is declared with the rest of this file's state,
// above). The library's command queue and pool are 8 slots shared with real
// commands (WalterModem.h:127,3501), which is why this guard exists at all:
// a stuck probe must not starve mqttConnect(). Written only by
// net_urc_probe() and probe_cb() below, both effectively single-threaded
// with respect to each other for any one probe (probe_cb() only ever runs
// after net_urc_probe() has already returned, whether asynchronously on the
// library's own task, or synchronously from inside net_urc_probe()'s own
// checkComm() call -- see probe_cb()'s doc comment).

// Runs on WalterModem's own task for a genuinely queued probe (checkComm()'s
// async contract), or synchronously on the calling task, from inside
// net_urc_probe()'s own checkComm() call, if the queue/pool was full
// (WalterDefines.h's _runCmd macro: cb != NULL still returns true from
// checkComm() itself in that case, but calls cb once with
// WALTER_MODEM_STATE_NO_MEMORY before checkComm() returns -- so
// checkComm()'s own return value cannot tell the two apart, only rsp->result
// here can). Sets flags/counters and nothing else: no logging, no AT calls,
// no net_check() registration semantics (docs/SLEEP_URC_DESIGN.md §5(1)).
// S10 (docs/SLEEP_URC_DESIGN.md §8.2, docs/SLEEP_URC_TASKS.md S10): the
// probe's own issue-to-answer elapsed time. Written by net_urc_probe() just
// before the checkComm() call and read/overwritten by probe_cb() -- safe by
// the same single-probe-at-a-time reasoning documented above probe_cb().
static int64_t s_probe_issue_us = 0;
static uint32_t s_probe_first_attempt_ms = 0;
// S18: the same elapsed time, but recorded only when the probe was genuinely
// ANSWERED, with a max and a count (net.h documents why the single
// last-writer-wins field above cannot be read as "the modem's answer
// latency"). This is the number the wake-window length is chosen from.
static uint32_t s_probe_answer_ms_last = 0;
static uint32_t s_probe_answer_ms_max = 0;
static uint32_t s_probe_answer_n = 0;

static void probe_cb(const WalterModemRsp *rsp, void *args)
{
    (void) args;
    uint32_t elapsed_ms = (uint32_t) ((esp_timer_get_time() - s_probe_issue_us) / 1000);
    s_probe_first_attempt_ms = elapsed_ms;
    if (rsp != NULL && rsp->result == WALTER_MODEM_STATE_OK) {
        s_probe_answer_ms_last = elapsed_ms;
        if (elapsed_ms > s_probe_answer_ms_max) {
            s_probe_answer_ms_max = elapsed_ms;
        }
        s_probe_answer_n++;
        net_probe_guard_answered(&s_probe_guard);
    } else if (rsp != NULL && rsp->result == WALTER_MODEM_STATE_NO_MEMORY) {
        net_probe_guard_noqueue(&s_probe_guard);
    } else {
        // 23 Sep S7 post-mortem: with PAGER_URC_PROBE_TIMEOUT_MS below this
        // branch is reachable in normal operation (an "AT" the modem did not
        // answer inside 2 s), so it must not land on noqueue() -- that
        // counter means "checkComm() could not queue it at all" and S7 reads
        // it that way. Counted `stuck` instead, same as an aged-out poll().
        net_probe_guard_failed(&s_probe_guard);
    }
}

extern "C" bool net_urc_probe(void)
{
    if (s_active_xport == NET_XPORT_WIFI || !s_modem_begun) {
        // no bare AT over WiFi; nothing to probe before begin()/mid-reset.
        // Counted (23 Sep S7b post-mortem, phaseAI-report.log:30): a window
        // whose whole report reads `probe_issued=0 probe_answered=2
        // probe_stuck=1` could not say whether the probe was never reached or
        // was silently switched off here for 80 wakes running.
        s_probe_skip_down++;
        return false;
    }
    if (!net_probe_guard_poll(&s_probe_guard)) {
        // Either still outstanding, or just declared stuck this wake (its
        // "OK" was lost in the flush burst, or it queued behind an
        // already-stalled command, docs/SLEEP_URC_DESIGN.md §1 item 5) --
        // the caller (modes.c) watches net_get_probe_counters().stuck for
        // the F4 escalation after 6 consecutive stuck probes, not this
        // function.
        return false;
    }
    if (net_publish_in_flight()) {
        // RCA_SLEEP_PUBLISH.md §1: a modem parked at a "> " data prompt would
        // consume this probe's "AT\r\n" as payload bytes, not a command.
        // The guard was never told a probe was issued, so it is still ready
        // to try again next wake.
        //
        // 23 Sep S7b post-mortem: adding net_connect_in_flight()/
        // net_modem_busy() here was considered and dropped as dead code --
        // this function is only ever called from inside modes.c's
        // `if (!skip_sleep)` branch, and skip_sleep already ORs all three
        // (modes.c:1986), so they are false by construction at this point.
        s_probe_skip_busy++;
        return false;
    }
    net_probe_guard_attempt(&s_probe_guard); // before the call: see probe_cb()'s doc comment
    // 23 Sep S7 post-mortem (build/bench-logs/phaseAF-report.log:27-28,33,41).
    // The probe used to inherit the library's 30 s x 3 default, which
    // contradicts docs/SLEEP_URC_DESIGN.md §5(1)'s "one AT, fire-and-forget,
    // no retries": an unanswered probe then held the library's single
    // in-flight command slot (_curCmd) for 30 s, and everything behind it
    // waited -- `stalled command: "AT" elapsed=30000 ms`, two `/up` acks at
    // 29 824 ms, 8 of 13 probes unanswered for >=4 wakes. The flush this
    // probe exists to trigger happens when the modem ACCEPTS the command,
    // not when we see its answer, so a short budget cannot lose a page; the
    // only thing given up is the (useless) knowledge that a late OK arrived.
    // 2 s is also deliberately shorter than the guard's own stuck bound
    // (NET_PROBE_GUARD_STUCK_WAKES x the wake interval, >=6 s even in ACTIVE
    // mode), so the library always releases the slot before the guard
    // reissues -- previously inverted, which is how a stuck probe could
    // occupy a second of the 8 shared queue slots for nothing.
    // Power: each avoided 30 s block is ~0.33 mAh of ESP awake time at 40 mA,
    // plus up to 15 s of publish_quiet's sleep-hold and up to 30 s of
    // net_modem_busy()'s, i.e. up to ~0.8 mAh per page (assumption: 40 mA
    // awake, 1 mA asleep -- docs/SLEEP_URC_DESIGN.md §2, not measured).
    // S10: stamped immediately before the call so even a synchronous
    // callback (the noqueue case) measures a valid, near-zero elapsed time.
    s_probe_issue_us = esp_timer_get_time();
    WalterModem::checkComm(NULL, probe_cb, NULL, PAGER_URC_PROBE_ATTEMPTS,
                           pdMS_TO_TICKS(PAGER_URC_PROBE_TIMEOUT_MS));
    if (s_probe_guard.outstanding) {
        // No synchronous net_probe_guard_noqueue() ran inside the call above,
        // so this was genuinely queued.
        net_probe_guard_issued(&s_probe_guard);
        return true;
    }
    return false; // probe_cb() already ran synchronously and counted noqueue
}

extern "C" net_probe_counters_t net_get_probe_counters(void)
{
    net_probe_counters_t out;
    out.issued = s_probe_guard.issued;
    out.answered = s_probe_guard.answered;
    out.stuck = s_probe_guard.stuck;
    out.noqueue = s_probe_guard.noqueue;
    out.timedout = s_probe_guard.timedout;
    out.skip_busy = s_probe_skip_busy;
    out.skip_down = s_probe_skip_down;
    out.first_attempt_ms = s_probe_first_attempt_ms;
    out.answer_ms_last = s_probe_answer_ms_last;
    out.answer_ms_max = s_probe_answer_ms_max;
    out.answer_n = s_probe_answer_n;
    return out;
}

extern "C" bool net_urc_probe_in_flight(void)
{
    // S18: read of the guard's single-slot `outstanding` flag. Set by
    // net_probe_guard_attempt() before checkComm() and cleared by whichever
    // of answered()/failed()/noqueue() probe_cb() reaches, so it goes false
    // exactly when the modem has answered (or the library has given up) --
    // which is the moment modes.c may stop holding the wake window open.
    return s_probe_guard.outstanding;
}

extern "C" uint32_t net_uart_rx_buffered_bytes(void)
{
    // RCA_SLEEP_URC.md fix 1's discriminator. PAGER_MODEM_UART is `static
    // constexpr` inside this file (private linkage), same reasoning as
    // net_sleep()'s own comment on why callers use the Kconfig pin macros
    // instead -- this accessor is the one modes.c needs instead.
    size_t len = 0;
    if (uart_get_buffered_data_len(PAGER_MODEM_UART, &len) != ESP_OK) {
        return 0;
    }
    return (uint32_t) len;
}

extern "C" bool net_take_registered_edge(void)
{
    bool e = s_reg_regained_edge;
    s_reg_regained_edge = false;
    return e;
}

extern "C" uint32_t net_unregistered_for_s(void)
{
    if (s_registered || s_unregistered_since_us == 0) {
        return 0;
    }
    return (uint32_t) ((esp_timer_get_time() - s_unregistered_since_us) / 1000000);
}

extern "C" bool net_recover_modem(void)
{
    // F4 ONLY. This is the one legal caller of reset() outside begin()'s
    // own cold-boot path (L5). Power effect: full modem power cycle +
    // re-attach - modes.c must rate-limit this to 1/10min.
    ESP_LOGI(TAG, "modem unresponsive: issuing hard reset + full re-init (F4)");
    s_modem_begun = false; // S1: net_urc_probe() must not queue against a modem mid-reset
    s_mqtt_connected = false;
    // v0.2 M1/M3: a physical reset wipes the modem's MQTT client outright,
    // so nothing is in flight and the fail streak that led here is moot --
    // reset both rather than let a stale streak immediately re-escalate.
    net_connect_guard_init(&s_connect_guard);
    publish_quiet_gate_init(&s_publish_quiet); // same reasoning: a reset drops any outstanding publish too
    net_probe_guard_init(&s_probe_guard); // same reasoning: the reset drops any outstanding probe too
    if (!WalterModem::reset()) {
        // 23 Sep S7b post-mortem (docs/SLEEP_URC_DESIGN.md §9): s_modem_begun
        // must NOT stay false here. This call can fail without the modem
        // being in a reset at all -- WalterModem::reset() waits for
        // "+SYSSTART", and _processModemRSP() finishes whatever command is
        // current on ANY error line regardless of its expected response
        // (WalterModem.cpp:2387-2408 sets result=ERROR, :4011-4013 then calls
        // _finishModemCMD(cmd, result) with no atRsp check at all), so one
        // orphaned "+CME ERROR: 4" from an earlier command fails this reset
        // immediately. Every other caller in this file keeps commanding the
        // modem after we return false (modes.c backs off and calls
        // net_session_up() again), so leaving the flag false silently
        // disables ONLY the per-wake URC drain probe -- the one mechanism
        // ~30 s page delivery depends on -- until the next F4, which
        // rate_limited_modem_recover() holds off for 10 minutes
        // (modes.c:1577). phaseAI-report.log is what that looks like:
        // probe_issued=0 over 80 wakes and no page received in 6 minutes.
        s_modem_begun = true;
        ESP_LOGI(TAG, "WalterModem::reset() failed");
        return false;
    }
    // WalterModem::begin() is documented to no-op on the 2nd+ call, so this
    // safely redoes opstate/PDP/eDRX/PSM. The reset wiped the modem's TLS
    // profile and MQTT client configuration; they are redone as soon as the
    // pager is registered. Short attach wait: this runs on the main loop.
    s_session_configured = false;
    s_registered = false;
    return net_bringup(20);
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

// docs/WIFI_TASKS.md W4: net_service_session() moved to xport_lte.cpp with
// the rest of the MQTT session code. This wrapper applies the LTE-only
// suppression (net_set_lte_suppressed() above, net.h's own doc comment)
// before dispatching -- a future WiFi transport's service tick must not be
// silenced by suppressions that are only about the modem.
extern "C" void net_service_session(void)
{
    if (s_active_xport == NET_XPORT_LTE && s_lte_suppressed) {
        return;
    }
    s_xport_ops->service();
}

extern "C" void net_get_mqtt_status(net_mqtt_status_t *out)
{
    s_xport_ops->status(out);
}

extern "C" void net_ack_disconnect_edge(void)
{
    s_xport_ops->ack_disconnect_edge();
}

extern "C" void net_ack_session_restart_edge(void)
{
    s_xport_ops->ack_session_restart_edge();
}

extern "C" bool net_modem_busy(void)
{
    return s_xport_ops->modem_busy();
}

extern "C" bool net_connect_in_flight(void)
{
    return s_xport_ops->connect_in_flight();
}

extern "C" bool net_publish_in_flight(void)
{
    // 23 Sep release-build fix (44-byte publish corruption, publish_quiet.h's
    // own module comment): true while a pager-originated publish's AT round
    // trip is outstanding AND still within its own PUBLISH_SLEEP_HOLD_MAX_US
    // (15s) window -- bounded the same way net_connect_in_flight()'s 30s
    // connect timeout is bounded, so a lost PUBLISHED URC cannot pin the
    // device awake forever.
    return publish_quiet_gate_hold_sleep(&s_publish_quiet, esp_timer_get_time());
}

extern "C" bool net_connect_fail_streak_maxed(void)
{
    return s_xport_ops->connect_fail_streak_maxed();
}

extern "C" uint32_t net_take_memfull_delta(void)
{
    return s_xport_ops->take_memfull_delta();
}

extern "C" uint32_t net_take_oversize_delta(void)
{
    return s_xport_ops->take_oversize_delta();
}

extern "C" const char *net_get_device_id(void)
{
    return ident_get_dev_id();
}

extern "C" net_pager_counters_t net_get_pager_counters(void)
{
    // RCA_SLEEP_PUBLISH.md §3 instrumentation: the vendored component's
    // counters (WalterDefines.h/PATCHES.md 1.12) are transport-independent
    // (they live inside the WalterModem C++ class this file always links
    // against, whatever net_xport_active() reports), so this reads them
    // directly rather than through the xport ops vtable. Power effect: none.
    walter_modem_pager_counters_t c = walter_modem_pager_counters();
    net_pager_counters_t out;
    out.datatx_retx = c.datatx_retx;
    out.prompt_orphan = c.prompt_orphan;
    out.buf_drop_queue = c.buf_drop_queue;
    out.buf_drop_pool = c.buf_drop_pool;
    // S3 (patch 1.13): see WalterDefines.h's own comment on
    // walter_modem_pager_counters_t for what each of these means.
    out.prompt_handled = c.prompt_handled;
    out.payload_bytes_written = c.payload_bytes_written;
    out.txdone_timeouts = c.txdone_timeouts;
    static_assert(sizeof(out.stall_cmd) == sizeof(c.stall_cmd), "net.h/WalterDefines.h stall_cmd size mismatch");
    memcpy(out.stall_cmd, c.stall_cmd, sizeof(out.stall_cmd));
    out.stall_elapsed_ms = c.stall_elapsed_ms;
    out.stall_cts_level = c.stall_cts_level;
    out.stall_tx_ring_bytes = c.stall_tx_ring_bytes;
    // S10 (docs/SLEEP_URC_DESIGN.md §8.2, docs/SLEEP_URC_TASKS.md S10): see
    // WalterDefines.h's own comment on walter_modem_pager_counters_t.
    out.rsp_no_cmd = c.rsp_no_cmd;
    out.payload_stuck_ms = c.payload_stuck_ms;
    return out;
}

extern "C" uint32_t net_get_publish_ring(net_publish_ring_entry_t *out, uint32_t cap)
{
    // RCA_SLEEP_PUBLISH.md §3 instrumentation: the ring lives in
    // xport_lte.cpp (the only transport this bug applies to); read directly,
    // same reasoning as net_get_pager_counters() above.
    return lte_get_publish_ring(out, cap);
}

extern "C" uint32_t net_get_resub_swallowed_count(void)
{
    // S2: same reasoning as net_get_publish_ring() above -- the counter
    // lives in xport_lte.cpp (the only transport this fix applies to).
    return lte_get_resub_swallowed_count();
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

// Debug console `at <command>`: sends one raw AT command and relies on the
// WalterModem debug trace to show the reply. Debug build only.
extern "C" bool net_debug_at(const char *cmd)
{
    return WalterModem::sendCmd(cmd);
}

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
// docs/ROADMAP.md's "temporary diagnostics" (nettest/mqtttest) no longer
// ship in the release binary -- debug build only from here down to
// net_check_mqtt()'s closing brace below.
static size_t s_nettest_pad_bytes = 0;

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
#endif /* PAGER_DEBUG_NO_LIGHT_SLEEP */

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

// PROTOCOL.md §13.2 `cell` (this task): the ITU/3GPP convention behind "US
// networks are 3 digits" -- NANP countries (Canada 302, USA 310-316, Puerto
// Rico 330, US Virgin Islands 332) assign a 3-digit MNC; used only as a
// fallback when PATCHES.md 1.9's ncDigits is unavailable/unparsed (0).
// UNVERIFIED for every MCC outside this table -- most of the rest of the
// world uses 2-digit MNCs, but this is not exhaustively confirmed, just the
// conventional default.
static bool is_nanp_mcc(uint16_t mcc)
{
    return mcc == 302 || (mcc >= 310 && mcc <= 316) || mcc == 330 || mcc == 332;
}

extern "C" bool net_get_cell_info(net_cell_info_t *out)
{
    if (s_cell_info_stale) {
        WalterModemRsp rsp = {};
        if (WalterModem::getCellInformation(WALTER_MODEM_SQNMONI_REPORTS_SERVING_CELL, &rsp) &&
            rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CELL_INFO) {
            const WalterModemCellInformation &ci = rsp.data.cellInformation;
            uint8_t mnc_digits = ci.ncDigits; // PATCHES.md 1.9
            if (mnc_digits != 2 && mnc_digits != 3) {
                mnc_digits = is_nanp_mcc(ci.cc) ? 3 : 2; // fallback, see is_nanp_mcc()'s own comment
            }
            // `cc` is a uint16_t in the vendor struct (no narrower type is
            // offered) even though a real MCC is always 3 digits; clamp so
            // "%03u" can never need more than 3 digits, both to satisfy
            // -Wformat-truncation and because a >=1000 reading is not a real
            // MCC -- loc.c's cell_shape_ok() would reject it anyway once it
            // is out of shape, so this only changes whether that happens
            // silently or with a log line here.
            unsigned cc = (unsigned) ci.cc;
            if (cc > 999) {
                ESP_LOGI(TAG, "cell info: implausible MCC %u from AT+SQNMONI (clamped)", cc);
                cc %= 1000u;
            }
            snprintf(s_cell_cache.mcc, sizeof(s_cell_cache.mcc), "%03u", cc);
            // This task: `nc` was widened to uint16_t (PATCHES.md 1.9) so a
            // 3-digit NANP MNC like "410" survives strToUint16() instead of
            // overflowing strToUint8()'s old UINT8_MAX check -- but the
            // compiler can no longer prove a uint16_t (up to 65535) fits
            // mnc[4] the way it could for the old uint8_t (always <=3
            // digits). The raw "Nc:" field is only ever 1-3 ASCII digits
            // (net.cpp's own patch to WalterModem.cpp's +SQNMONI parser
            // gates `ncDigits` on exactly that), so `nc` itself can never
            // actually exceed 999 -- clamp defensively anyway, same
            // %03u-truncation-and-implausible-value reasoning `cc` above
            // already uses, both to satisfy -Wformat-truncation and because
            // a >=1000 reading is not a real MNC either.
            unsigned nc = (unsigned) ci.nc;
            if (nc > 999) {
                ESP_LOGI(TAG, "cell info: implausible MNC %u from AT+SQNMONI (clamped)", nc);
                nc %= 1000u;
            }
            snprintf(s_cell_cache.mnc, sizeof(s_cell_cache.mnc), "%0*u", (int) mnc_digits, nc);
            s_cell_cache.tac = ci.tac;
            s_cell_cache.ci = ci.cid & 0x0FFFFFFFu; // 28 bits, PROTOCOL.md §13.2
            s_cell_cache.have_rsrp = (ci.rsrp <= -30.0f && ci.rsrp >= -156.0f);
            s_cell_cache.rsrp = s_cell_cache.have_rsrp ? (int) (ci.rsrp - 0.5f) : 0;
            s_cell_cache.valid = true;
            s_cell_info_stale = false;
            ESP_LOGI(TAG, "cell info refreshed: mcc=%s mnc=%s(%u digits, %s) tac=%u ci=%u rsrp=%d",
                     s_cell_cache.mcc, s_cell_cache.mnc, (unsigned) mnc_digits,
                     (ci.ncDigits == mnc_digits) ? "raw" : "NANP fallback", (unsigned) s_cell_cache.tac,
                     (unsigned) s_cell_cache.ci, s_cell_cache.have_rsrp ? s_cell_cache.rsrp : 0);
        } else {
            // Deliberately NOT clearing s_cell_info_stale: retry on the next
            // call rather than caching a failure. §13.2: "if the cell cannot
            // be read, send the answer without it" -- if no earlier good
            // reading exists, s_cell_cache.valid stays false and the caller
            // omits `cell`; if one does exist, it is served stale rather than
            // dropped (logged either way).
            ESP_LOGI(TAG, "getCellInformation() failed; /loc will %s",
                     s_cell_cache.valid ? "use the last known cell (stale)" : "omit cell");
        }
    }
    if (out) {
        *out = s_cell_cache;
    }
    return s_cell_cache.valid;
}

extern "C" void net_set_accel_wake(bool on)
{
    s_accel_wake_enabled = on;
}

extern "C" void net_enable_accel_wake(void)
{
    net_set_accel_wake(true);
}

extern "C" uint32_t net_get_ext1_wakes(void)
{
    return s_ext1_wakes;
}

// ---------------------------------------------------------------------------
// v0.2 §4.4 (CA trust, cafetch.c). See net.h's own doc comments for the
// contract each of these follows.
// ---------------------------------------------------------------------------

extern "C" bool net_ca_fetch_open(const char *host, uint16_t port)
{
    // v0.2 bug fix #4 (§2.4)/GOTCHAS.md's rule applies to this profile
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

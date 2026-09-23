// xport_lte.cpp — the LTE-M MQTT transport (docs/WIFI_DESIGN.md §1,
// docs/WIFI_TASKS.md W4). This is the MQTT session code that used to live in
// net.cpp, moved here **verbatim** behind net_xport.h's ops vtable: every
// function body below is byte-identical to its pre-move counterpart in
// net.cpp except for (a) its name, prefixed `lte_` to match the vtable field
// it is wired to instead of colliding with net.cpp's now-dispatching
// net_*() wrapper of the same historical name, and (b) internal calls to a
// sibling that was itself renamed for the same reason (net_session_down() ->
// lte_session_down()). No logic, no constant, no log line, and no comment
// was edited. See net_internal.h for the small amount of net.cpp state this
// file still reads/writes directly (and vice versa) — the mechanical cost of
// the translation-unit split, not a behaviour change.
//
// C++ for the same reason net.cpp is: dptechnics/walter-modem v1.5.0 is a
// C++ class (WalterModem, static methods, no extern "C"). See net.h.
//
// Authority: docs/PROTOCOL.md §6 (session/keepalive/eDRX), §8 (wake sources).
// All power-effect comments here are PENDING_HW, unchanged from net.cpp.

#include "net.h"
#include "net_xport.h"
#include "net_internal.h"
#include "ident.h"
#include "net_connect_guard.h"
#include "publish_quiet.h"

#include "WalterModem.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";

// MQTT keepalive. Measured 2026-09-21 on AT&T (US Mobile), docs/V02_DESIGN.md
// §9.1: on this modem firmware (LR8.2.1.0-61488) the AT+SQNSMQTTCONNECT
// keepalive parameter does NOT make the modem send a PINGREQ -- three idle
// sessions, zero PINGREQs in 8+ minutes each. This constant only sets the
// broker's own drop deadline (measured 1.5x keepalive). Keeping the flow/NAT
// alive and detecting a dead session early is PAGER_MQTT_PING_S's job
// (net_service_session(), §9.2/§9.4), not this one's. 480 s keeps "broker
// declares a dead pager offline" at <=12 min.
static constexpr uint16_t PAGER_MQTT_KEEPALIVE_S = 480;

// §9.2/§9.4: idle-uplink liveness ping interval -- net_service_session()
// re-SUBSCRIBEs to the down-topic every this-many seconds of uplink silence,
// which is both what keeps the broker/NAT flow alive (the modem itself does
// not, see PAGER_MQTT_KEEPALIVE_S's comment above) and the mechanism that
// repairs a modem-initiated silent resume (§9.1 item 2). 300 s halves the
// shortest observed idle death (10.5 min) and leaves 420 s of margin to the
// broker's 720 s timeout -- one whole missed ping is survivable (§9.2).
static constexpr uint32_t PAGER_MQTT_PING_S = 300;

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

// net.cpp's net_init()/net_recover_modem() reset these fresh on boot/F4
// recovery, and the PAGER_DEBUG_NO_LIGHT_SLEEP-only mqtttest diagnostic
// reads s_mqtt_connected/s_disconnect_edge directly -- see net_internal.h.
volatile bool s_mqtt_connected = false;
volatile bool s_disconnect_edge = false;
static volatile int s_last_rc = 0;
static volatile net_mqtt_rc_class_t s_last_class = NET_MQTT_RC_NONE;

// v0.2 bug fixes M1 (22 Sep outage: "CONNECT issued, no CONNECTED seen" bound)
// / M3 (22 Sep evening: escalate after 3 consecutive net_session_up()
// failures) — see net_connect_guard.h's own module comment. Written from
// net_session_up() (this file's own task) and from pager_mqtt_event_handler()
// (_eventProcessingTask) via net_connect_guard_clear() -- same "plain
// struct of single-word fields, no mutex, races tolerated" reasoning as the
// rest of this block (net_connect_guard.c's writes are each a single bool/
// int64_t/uint32_t store, same as s_mqtt_connected etc. above).
net_connect_guard_t s_connect_guard;

// 23 Sep display-corruption field failures — see publish_quiet.h's own
// module comment. Written from net_publish()/net_publish_raw() (whichever
// task called them) on issue, and from pager_mqtt_event_handler()
// (_eventProcessingTask) on WALTER_MODEM_MQTT_EVENT_PUBLISHED -- same
// "plain struct, no mutex, races tolerated" reasoning as s_connect_guard
// above (a torn read here costs at worst one extra/missed refresh delay,
// never a correctness bug -- disp.c's own refresh path is what actually
// matters for the panel).
publish_quiet_gate_t s_publish_quiet;

// RCA_SLEEP_PUBLISH.md §3 instrumentation: a 12-entry RAM ring of the most
// recent net_publish()/net_publish_raw() attempts (topic tail, length, issue
// time, outcome, blocking-call elapsed time), read by modes.c's sleeptest
// report via net_get_publish_ring() (net.cpp forwards to
// lte_get_publish_ring() below, net_internal.h). RAM only, not
// RTC_DATA_ATTR: sleeptest only light-sleeps and reads this right before its
// own deliberate hard reset (the report is saved to NVS text before that
// reset), never across a real deep sleep. No behaviour change: this only
// records the outcome of a publish this file was already going to attempt.
#define PUBLISH_RING_N NET_PUBLISH_RING_MAX
static net_publish_ring_entry_t s_publish_ring[PUBLISH_RING_N];
static uint32_t s_publish_ring_next = 0;  // next slot to write (wraps)
static uint32_t s_publish_ring_count = 0; // number of valid entries, saturates at PUBLISH_RING_N

static void publish_ring_record(const char *topic, uint16_t len, int64_t issued_us,
                                 WalterModemState result, int64_t elapsed_us)
{
    net_publish_ring_entry_t *e = &s_publish_ring[s_publish_ring_next];
    e->issued_us = issued_us;
    size_t tlen = strlen(topic);
    const char *tail = (tlen > sizeof(e->topic_tail) - 1) ? topic + tlen - (sizeof(e->topic_tail) - 1) : topic;
    snprintf(e->topic_tail, sizeof(e->topic_tail), "%s", tail);
    e->len = len;
    e->outcome = (result == WALTER_MODEM_STATE_OK)        ? NET_PUBLISH_RING_OK
                 : (result == WALTER_MODEM_STATE_TIMEOUT) ? NET_PUBLISH_RING_TIMEOUT
                                                           : NET_PUBLISH_RING_ERROR;
    e->elapsed_ms = (uint32_t) (elapsed_us / 1000);
    s_publish_ring_next = (s_publish_ring_next + 1) % PUBLISH_RING_N;
    if (s_publish_ring_count < PUBLISH_RING_N) {
        s_publish_ring_count = s_publish_ring_count + 1;
    }
}

// net_internal.h declares this for net.cpp's net_get_publish_ring() to call.
// Returns entries oldest-first.
uint32_t lte_get_publish_ring(net_publish_ring_entry_t *out, uint32_t cap)
{
    uint32_t n = (s_publish_ring_count < cap) ? s_publish_ring_count : cap;
    // Oldest valid entry is s_publish_ring_next when the ring is full;
    // when it is not yet full, the oldest entry is always slot 0.
    uint32_t start = (s_publish_ring_count < PUBLISH_RING_N) ? 0 : s_publish_ring_next;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_publish_ring[(start + i) % PUBLISH_RING_N];
    }
    return n;
}

// v0.2 §9.4 (session liveness / silent-resume repair, net_service_session()).
// All written from pager_mqtt_event_handler() (_eventProcessingTask) and/or
// net_service_session() (modes_run()'s own task) -- same "single-word,
// volatile, no mutex" reasoning as the rest of this block's comment above.
static volatile int64_t s_last_uplink_us = 0;   // last publish send or SUBACK, esp_timer_get_time()
static volatile bool s_resub_pending = false;   // a modem-initiated resume needs a raw re-SUBSCRIBE
static volatile bool s_resub_wait = false;      // raw re-SUBSCRIBE sent, waiting on its SUBACK
static volatile int64_t s_resub_sent_us = 0;    // when the outstanding raw re-SUBSCRIBE was sent
static volatile bool s_session_restart_edge = false; // set once the resume repair's SUBACK lands
// Not in the spec's own static list, but required to implement its step 4
// correctly: net_service_session() clears s_resub_pending the moment it
// *sends* the raw re-SUBSCRIBE (so it does not re-send every iteration while
// s_resub_wait is true), so by the time the matching SUBACK arrives
// s_resub_pending can no longer tell the SUBSCRIBED handler whether that
// SUBACK is closing a resume repair (-> session_restart_edge) or an ordinary
// idle-timeout liveness ping (-> nothing further). This single-shot flag
// carries that classification across the round trip.
static volatile bool s_resub_is_resume = false;

static volatile bool s_handler_busy = false; // true while the MQTT event
                                             // handler is inside an AT
                                             // transaction or the app
                                             // callback (see the RTS interlock below)

static volatile uint32_t s_memfull_count = 0;
static volatile uint32_t s_oversize_count = 0;

// Sized to the larger of the two namespace caps above; sized once, no
// malloc on the RX path (L9).
static uint8_t s_mqtt_rx_buf[PAGER_BOOT_MAX_PAYLOAD];

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

// Runs on WalterModem's _eventProcessingTask (WalterModem.cpp:1595-1626), NOT
// the RX task/ISR. Calling modem APIs from here is the vendor's own pattern
// (examples/mqtts). Keep this short (L4). Registered from net.cpp's
// net_bringup()/net_bootstrap_attach() via WalterModem::setMQTTEventHandler()
// (net_internal.h declares it so those two call sites keep compiling).
void pager_mqtt_event_handler(WMMQTTEventType event, const WMMQTTEventData *data, void *args)
{
    (void) args;

    switch (event) {
    case WALTER_MODEM_MQTT_EVENT_CONNECTED:
        // v0.2 M1/M2: a SUCCESSFUL CONNECTED does NOT clear the guard -- the
        // connect stays "in flight" until SUBSCRIBED, because s_mqtt_connected
        // is only set there and modes.c's retry branch keys on it: with the
        // guard cleared here, the CONNECTED->SUBSCRIBED window (~150 ms on
        // the bench) read as "not connected, nothing in flight" and a second
        // AT+SQNSMQTTCONNECT went out, answered +CME ERROR: 4 (phase1-boot.log
        // 121605-121655, and every boot before it). A FAILED CONNECTED is a
        // real answer, so it does clear the guard and the ordinary rc
        // classification below takes over.
        if (data->rc != WALTER_MODEM_MQTT_SUCCESS) {
            net_connect_guard_clear(&s_connect_guard);
            s_last_rc = data->rc;
            s_last_class = classify_mqtt_rc(data->rc);
            s_disconnect_edge = true;
            ESP_LOGI(TAG, "MQTT connect failed, rc=%d", data->rc);
            break;
        }
        if (s_mqtt_connected) {
            // v0.2 §9.4 step 2: a CONNECTED event while we already believe
            // we are connected, with no DISCONNECTED in between, is a
            // modem-initiated silent resume (§9.1 item 2) -- the AT manual's
            // own words: "If the MQTT connection was dropped by the server
            // and automatically resumed by the modem ... the MCU must
            // re-subscribe". Do NOT call mqttSubscribe() here: mqttConnect()
            // is the only thing that frees the vendor's local topic table,
            // and a modem-initiated resume never calls it, so mqttSubscribe()
            // would just dedupe this into a silent no-op ("Topic already in
            // use", WalterMQTT.cpp:120-123) -- nothing would ever go out on
            // the wire. Queue it instead; net_service_session() (modes.c's
            // task) sends the repair as a raw AT+SQNSMQTTSUBSCRIBE.
            // s_mqtt_connected is left alone: publishes still work.
            ESP_LOGI(TAG, "MQTT session resumed by the modem (no DISCONNECTED seen); "
                          "re-subscribe queued");
            s_resub_pending = true;
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
        // v0.2 M1/M2: THE clearing point for a successful connect -- the
        // session is usable from here, and s_mqtt_connected (set below)
        // takes over as modes.c's "no retry needed" signal. See the
        // CONNECTED case above for why it is not cleared earlier.
        net_connect_guard_clear(&s_connect_guard);
        if (data->rc != WALTER_MODEM_MQTT_SUCCESS) {
            ESP_LOGI(TAG, "MQTT subscribe failed, rc=%d", data->rc);
            break;
        }
        s_mqtt_connected = true;
        // v0.2 §9.4 step 4: this SUBACK proves the round trip, whether it
        // answered the ordinary post-CONNECTED subscribe above or a raw
        // liveness-ping/resume-repair re-SUBSCRIBE from net_service_session()
        // -- either way the flow is alive right now.
        s_last_uplink_us = esp_timer_get_time();
        if (s_resub_wait) {
            s_resub_wait = false;
            if (s_resub_is_resume) {
                s_resub_is_resume = false;
                s_session_restart_edge = true;
            }
        }
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
            s_last_uplink_us = esp_timer_get_time(); // §9.4: uplink activity resets the idle clock
        }
        // §4.1 r6 / §4.2: freeing the matching pending_ack/pending_up RTC
        // entry belongs to msg.c, which is the thing that knows
        // which entry a given publish was for. No such entries exist yet.
        //
        // 23 Sep fix: this is the completion event for the publish's AT
        // round trip (the +SQNSMQTTONPUBLISH URC, or the terminal OK/ERROR)
        // regardless of rc — either way the uplink this publish caused is
        // over, so clear it from the in-flight count and arm the quiet
        // window disp.c's pre-refresh gate waits on (publish_quiet.h).
        publish_quiet_gate_done(&s_publish_quiet, esp_timer_get_time());
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
        // A liveness ping or resume repair in flight is void now: the next
        // mqttConnect() subscribes afresh, so nothing must carry over.
        s_resub_wait = false;
        s_resub_pending = false;
        s_resub_is_resume = false;
        ESP_LOGI(TAG, "MQTT disconnected, rc=%d", data->rc);
        break;

    case WALTER_MODEM_MQTT_EVENT_MEMORY_FULL:
        s_memfull_count = s_memfull_count + 1; // volatile: avoid deprecated ++ (C++20)
        ESP_LOGI(TAG, "MQTT modem buffer MEMORY_FULL (count this cycle=%u)",
                 (unsigned) s_memfull_count);
        break;
    }
}

static bool lte_session_up(void)
{
    // Power effect: one TLS handshake, ~5kB (PROTOCOL.md §7.2/§7.3), plus
    // the RRC time it takes. Never call this on a timer - only after
    // net_init() and after a detected session loss (F3).
    if (!s_registered) {
        // A poll, not only the URC: the URC can be missed while the ESP32 sleeps.
        note_registration(net_is_attached());
        if (!s_registered) {
            ESP_LOGI(TAG, "no network: not connecting yet");
            return false;
        }
    }
    if (!s_session_configured && !configure_session()) {
        // v0.2 M3 (phaseO-recover.log): mqttConfig() answering +CME ERROR 4
        // because the modem's own MQTT client is still up is exactly this
        // failure. The fix is proactive: whichever code path decides the
        // session is dead calls net_session_down() BEFORE this function runs
        // again (net_service_session()'s liveness-dead and M1 connect-timeout
        // branches). Repeated failures here count toward the 3-strike modem
        // recover in modes.c (net_connect_fail_streak_maxed()), which covers
        // a client left up by anything else (e.g. an ESP-only reset).
        net_connect_guard_note_fail(&s_connect_guard);
        return false;
    }
    s_disconnect_edge = false;
    if (!WalterModem::mqttConnect(ident_get_host(), ident_get_port(), PAGER_MQTT_KEEPALIVE_S)) {
        ESP_LOGI(TAG, "mqttConnect() call could not be queued");
        // If the modem has lost its client configuration (it answers +CME
        // ERROR), the next attempt redoes it. Cheap, and it is the state that
        // used to be unrecoverable.
        s_session_configured = false;
        net_connect_guard_note_fail(&s_connect_guard); // v0.2 M3: 3 in a row -> modem recover
        return false;
    }
    ESP_LOGI(TAG, "MQTT connect issued to %s:%u", ident_get_host(), (unsigned) ident_get_port());
    // v0.2 M1: arms the 30s "no CONNECTED/SUBSCRIBED seen" bound
    // (net_service_session() below) and, per net_connect_guard_issued()'s own
    // contract, resets the M3 fail streak -- the modem accepted CONFIG/CONNECT
    // this time, whatever happens next.
    net_connect_guard_issued(&s_connect_guard, esp_timer_get_time());
    return true;
}

static void lte_session_down(void)
{
    // F3 recovery path only (never called on a timer or speculatively) --
    // plus, as of v0.2 M3, net_service_session()'s own host-detected-dead
    // branches below.
    WalterModem::mqttDisconnect();
    s_mqtt_connected = false;
    net_connect_guard_clear(&s_connect_guard); // v0.2 M1: no connect is in flight once torn down
}

static bool lte_publish(const char *topic, char *buf, uint16_t len, uint8_t qos)
{
    if (len > PAGER_MAX_PAYLOAD) {
        ESP_LOGI(TAG, "refusing to publish %u bytes > %u cap (PROTOCOL.md §3.3)",
                 (unsigned) len, (unsigned) PAGER_MAX_PAYLOAD);
        return false;
    }
    // RCA_SLEEP_PUBLISH.md §4 item 3: mark the publish in flight BEFORE the
    // command is queued, not after mqttPublish() returns. mqttPublish() is
    // synchronous (no cb passed, _returnAfterReply() blocks this call for
    // the whole AT round trip) -- the old ordering called
    // publish_quiet_gate_issued() only once that whole round trip had
    // already completed, so the gate never actually covered the transaction
    // it exists to protect (985a343's no-op, per the RCA §1 table). Moving
    // it here closes the event-task publish window (real, previously
    // unobserved -- setup.c:827 and modes.c's set_mode()->publish_status_online()
    // both call in from the modem event task); it does NOT fix the
    // corrupted-publish bug itself -- RCA §1 shows the ack publish this bug
    // hits blocks the *modes* task for the full timeout and cannot reach
    // net_sleep() regardless of this gate. That bug is patches 1.11/1.12.
    int64_t issued_us = esp_timer_get_time();
    publish_quiet_gate_issued(&s_publish_quiet, issued_us);
    WalterModemRsp rsp = {};
    // L6: mqttPublish() takes non-const uint8_t*; publish from a mutable buffer.
    bool ok = WalterModem::mqttPublish(topic, (uint8_t *) buf, len, qos, &rsp);
    if (ok) {
        s_last_uplink_us = esp_timer_get_time(); // §9.4: successful publish resets the idle clock
        // matching publish_quiet_gate_done() call is the PUBLISHED event
        // handler above (the later +SQNSMQTTONPUBLISH URC/OK/ERROR).
    } else {
        // The AT command itself never queued successfully (synchronous
        // ERROR/TIMEOUT) -- no PUBLISHED URC will ever arrive to clear this
        // one, so undo the issued() above here instead of leaking the
        // in-flight count for up to PUBLISH_SLEEP_HOLD_MAX_US.
        publish_quiet_gate_done(&s_publish_quiet, esp_timer_get_time());
    }
    publish_ring_record(topic, len, issued_us, rsp.result, esp_timer_get_time() - issued_us);
    return ok;
}

static bool lte_publish_raw(const char *topic, uint8_t *buf, uint16_t len, uint8_t qos)
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
    //
    // RCA_SLEEP_PUBLISH.md §4 item 3: same issued-before-queued ordering as
    // lte_publish() above, see that function's own comment for why.
    int64_t issued_us = esp_timer_get_time();
    publish_quiet_gate_issued(&s_publish_quiet, issued_us);
    WalterModemRsp rsp = {};
    bool ok = WalterModem::mqttPublish(topic, buf, len, qos, &rsp);
    if (ok) {
        s_last_uplink_us = esp_timer_get_time(); // §9.4: successful publish resets the idle clock
        // matching publish_quiet_gate_done() call is the PUBLISHED event
        // handler above.
    } else {
        // See lte_publish()'s own comment: no PUBLISHED URC is coming for a
        // publish that never queued successfully, so undo issued() here.
        publish_quiet_gate_done(&s_publish_quiet, esp_timer_get_time());
    }
    publish_ring_record(topic, len, issued_us, rsp.result, esp_timer_get_time() - issued_us);
    return ok;
}

static uint32_t lte_publish_quiet_wait_ms(uint32_t max_wait_ms)
{
    // 23 Sep display-corruption fix (publish_quiet.h's own module comment):
    // disp.c's pre-refresh gate hook (ui.c's strong disp_pre_write_gate_hook())
    // calls this before any panel SPI command goes out. Bounded so a lost
    // PUBLISHED event (a dropped URC) cannot stall rendering forever --
    // matches disp.c's own "NEVER a tight busy-loop"/bounded-wait discipline
    // (disp_wait_busy_fb()).
    uint32_t waited_ms = 0;
    while (publish_quiet_gate_should_wait(&s_publish_quiet, esp_timer_get_time())) {
        if (waited_ms >= max_wait_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // NEVER a tight busy-loop
        waited_ms += 10;
    }
    return waited_ms;
}

// v0.2 §9.4: idle-uplink liveness ping / silent-resume repair. Called once
// per wake-and-drain iteration from modes_run()'s own task (never from an
// event callback), guarded there by the same three suppressions the
// reconnect path already honours. Power effect: none when neither condition
// below holds; otherwise one AT round trip (RRC time for one subscribe if
// the modem was idle) at most once per PAGER_MQTT_PING_S.
static void lte_service_session(void)
{
    int64_t now = esp_timer_get_time();

    // v0.2 M1 (22 Sep outage): net_session_up() issued a connect but neither
    // CONNECTED nor SUBSCRIBED has arrived within NET_CONNECT_TIMEOUT_US
    // (30s -- matches the Step 5 SUBACK bound just below). esp_timer_get_time()
    // keeps counting across light sleep, and this function is serviced once
    // per wake-and-drain iteration (modes.c), so the bound is honoured even
    // when most of it elapses asleep -- exactly the "one missed event must
    // not hang forever" fix the outage needed. Same M3 treatment as Step 5
    // below: net_session_down() runs HERE, at the moment the host makes the
    // "dead" call, not left for whichever retry branch runs next (see that
    // branch's own comment for why this is the right place, not modes.c's
    // retry branch).
    if (net_connect_guard_check_timeout(&s_connect_guard, now)) {
        s_mqtt_connected = false;
        s_last_class = NET_MQTT_RC_TRANSIENT;
        ESP_LOGI(TAG, "connect watchdog: no CONNECTED/SUBSCRIBED within %llds of the connect - "
                      "treating the MQTT session as dead",
                 (long long) (NET_CONNECT_TIMEOUT_US / 1000000));
        lte_session_down(); // v0.2 M3: tear down the modem's client before F1/F3 backoff retries
        s_disconnect_edge = true;
        return;
    }

    // Step 5: no SUBACK within 30s (two wake cycles plus RRC setup) means the
    // session is dead -- far earlier than the modem's own ~6 minute silent
    // resume. mqttConnect() (net_session_up(), driven by the ordinary F1/F3
    // backoff this disconnect edge triggers) frees the topic table, so the
    // next subscribe after a real reconnect is a normal one.
    if (s_resub_wait && (now - s_resub_sent_us) > 30 * 1000000LL) {
        s_mqtt_connected = false;
        s_last_class = NET_MQTT_RC_TRANSIENT;
        s_resub_wait = false;
        s_resub_is_resume = false;
        ESP_LOGI(TAG, "liveness ping got no SUBACK in 30 s: treating the MQTT session as dead");
        // v0.2 M3 fix (tonight's phaseO-recover.log): this branch used to
        // only set s_disconnect_edge and rely on modes.c's F1/F3 backoff to
        // call net_session_up() again -- which then hit exactly tonight's
        // failure, mqttConfig() answering +CME ERROR: 4 because the modem's
        // own MQTT client was still connected (this host-side verdict never
        // told the modem otherwise). Option 1 of the two the spec offered:
        // tear the client down HERE, in the branch that makes the
        // host-detected-dead call, rather than in modes.c's retry branch --
        // by the time modes.c observes disconnect_edge, it can no longer
        // tell a host-detected loss from a modem-reported one, but net.cpp
        // can, right here, for free.
        lte_session_down();
        s_disconnect_edge = true;
        return;
    }

    bool resume_repair = s_resub_pending;
    bool idle_ping = s_mqtt_connected && !s_resub_wait &&
                      (now - s_last_uplink_us) >= (int64_t) PAGER_MQTT_PING_S * 1000000LL;
    if (!resume_repair && !idle_ping) {
        return;
    }

    int64_t idle_s = (now - s_last_uplink_us) / 1000000;

    // Raw AT+SQNSMQTTSUBSCRIBE, same WalterModem::sendCmd() path
    // net_debug_at() uses: WalterModem::mqttSubscribe() would silently no-op
    // this (WalterMQTT.cpp:120-123, "Topic already in use") since the topic
    // is already in the modem's local table from the very first connect --
    // for both the resume-repair case and the routine liveness ping.
    char cmd[32 + sizeof(s_down_topic)];
    snprintf(cmd, sizeof(cmd), "AT+SQNSMQTTSUBSCRIBE=0,\"%s\",1", s_down_topic);
    if (!WalterModem::sendCmd(cmd)) {
        ESP_LOGI(TAG, "liveness ping: re-SUBSCRIBE could not be sent (idle %llds)%s",
                 (long long) idle_s, resume_repair ? " (resume repair)" : "");
        return;
    }
    s_resub_sent_us = now;
    s_resub_wait = true;
    s_resub_pending = false;
    s_resub_is_resume = resume_repair;
    s_last_uplink_us = now;
    if (resume_repair) {
        ESP_LOGI(TAG, "liveness ping: re-SUBSCRIBE sent (resume repair)");
    } else {
        ESP_LOGI(TAG, "liveness ping: re-SUBSCRIBE sent (idle %llds)", (long long) idle_s);
    }
}

static void lte_get_mqtt_status(net_mqtt_status_t *out)
{
    if (!out) {
        return;
    }
    out->mqtt_connected = s_mqtt_connected;
    out->disconnect_edge = s_disconnect_edge;
    out->last_rc = s_last_rc;
    out->last_class = s_last_class;
    out->session_restart_edge = s_session_restart_edge;
}

static void lte_ack_disconnect_edge(void)
{
    s_disconnect_edge = false;
}

static void lte_ack_session_restart_edge(void)
{
    s_session_restart_edge = false;
}

static bool lte_modem_busy(void)
{
    return s_handler_busy;
}

static bool lte_connect_in_flight(void)
{
    // v0.2 M1/M2: true from net_session_up()'s successful mqttConnect() queue
    // until CONNECTED/SUBSCRIBED arrives, net_session_down() runs, or the 30s
    // connect timeout fires (net_service_session()) -- see net_connect_in_flight()'s
    // own doc comment in net.h for why this is a separate accessor from
    // net_modem_busy(), not folded into it.
    return net_connect_guard_in_flight(&s_connect_guard);
}

static bool lte_connect_fail_streak_maxed(void)
{
    // v0.2 M3: true once net_session_up() has failed NET_SESSION_UP_FAIL_ESCALATE
    // times in a row at the mqttConfig()/mqttConnect() step.
    return net_connect_guard_should_escalate(&s_connect_guard);
}

static uint32_t lte_take_memfull_delta(void)
{
    uint32_t v = s_memfull_count;
    s_memfull_count = 0;
    return v;
}

static uint32_t lte_take_oversize_delta(void)
{
    uint32_t v = s_oversize_count;
    s_oversize_count = 0;
    return v;
}

static const net_xport_ops_t s_lte_ops = {
    .up = lte_session_up,
    .down = lte_session_down,
    .publish = lte_publish,
    .publish_raw = lte_publish_raw,
    .publish_quiet_wait_ms = lte_publish_quiet_wait_ms,
    .service = lte_service_session,
    .status = lte_get_mqtt_status,
    .ack_disconnect_edge = lte_ack_disconnect_edge,
    .ack_session_restart_edge = lte_ack_session_restart_edge,
    .modem_busy = lte_modem_busy,
    .connect_in_flight = lte_connect_in_flight,
    .connect_fail_streak_maxed = lte_connect_fail_streak_maxed,
    .take_memfull_delta = lte_take_memfull_delta,
    .take_oversize_delta = lte_take_oversize_delta,
};

const net_xport_ops_t *xport_lte_ops(void)
{
    return &s_lte_ops;
}

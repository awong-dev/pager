/* net.h — C-linkage facade over net.cpp.
 *
 * net.cpp is C++ because dptechnics/walter-modem v1.5.0 exposes a C++ class
 * (WalterModem, static methods, no extern "C"). Everything that touches
 * WalterModem lives behind this header so modes.c / msg.c / ui.c / main.c
 * stay plain C. See docs/PROTOCOL.md §6, §8.
 *
 * The network core.
 * modes.c owns the RTC struct and all mode/backoff/retry policy; net.cpp
 * owns nothing that must survive a reset except via the small delta
 * counters below, which modes.c folds into its RTC-resident totals every
 * wake cycle.
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Classification of the last MQTT connect/disconnect outcome, for the F3
 * recovery policy in modes.c (docs/PROTOCOL.md §6.1, §8 failure modes). */
typedef enum {
    NET_MQTT_RC_NONE = 0,      /* no CONNECTED/DISCONNECTED event observed yet */
    NET_MQTT_RC_OK,
    NET_MQTT_RC_TRANSIENT,     /* CONN_LOST(-7) / NO_CONN(-4) -> 5/15/60/300s backoff */
    NET_MQTT_RC_PERMANENT,     /* CONN_REFUSED(-5) / AUTH(-11) / ACL_DENIED(-12) -> 300s steady */
    NET_MQTT_RC_TLS_FAIL,      /* WALTER_MODEM_MQTT_TLS(-8) -> provisioning bug, 300s steady */
} net_mqtt_rc_class_t;

typedef struct {
    bool                 mqtt_connected;   /* true from SUBSCRIBED(rc==0) until the next DISCONNECTED */
    bool                 disconnect_edge;  /* set once per fresh loss; caller must ack it */
    int                  last_rc;          /* raw WMMQTTConnRC from the event that set the edge */
    net_mqtt_rc_class_t  last_class;       /* classification of last_rc, see F3 */
} net_mqtt_status_t;

/* Cold-boot modem/network bring-up: begin() + handlers + opstate + PDP +
 * eDRX/PSM + attach + clock seed + TLS provisioning + mqttConfig.
 * Call once, from modes_boot(). Safe to call again after net_recover_modem()
 * (WalterModem::begin() is a documented no-op on the second call).
 * Power effect: takes the modem from reset to FULL opstate with an LTE-M
 * attach and eDRX 20.48s/2.56s requested; current rises to the (vendor,
 * PENDING_HW) attach transient then settles to the idle-attached floor. */
bool net_init(void);

/* Configures TLS profile 3 (bootstrap only) with
 * WALTER_MODEM_TLS_VALIDATION_NONE, for the one-time bootstrap MQTT hop of
 * docs/DEVICE_PLAN.md §3.2 step 3 (setup.c, F3.5). Does not touch profile 2
 * (production, CA-pinned to cert slot 12) or any cert slot — the bootstrap
 * bundle is authenticated/encrypted under a single-use key derived from the
 * typed setup code, so no CA is needed on this hop. Call once before the
 * bootstrap mqttConfig()/mqttConnect(); switching back to profile 2 for the
 * production session is the caller's job (another mqttConfig() call).
 * Power effect: one AT command, no RRC of its own. */
bool net_tls_profile_bootstrap(void);

/* ---------------------------------------------------------------------
 * Bootstrap-only additions (docs/DEVICE_PLAN.md §3.2, setup.c, F3.5).
 *
 * Not in F3.5's `Files` list, but added here anyway and flagged in the F3.5
 * report: net.h/net.cpp is the only place allowed to touch WalterModem (see
 * this header's own module comment), and DEVICE_TASKS.md F3.5's "Do" step
 * ("attach -> profile 3 -> MQTT as boot-{bid} -> subscribe ... -> publish
 * ... -> disconnect") cannot be implemented from setup.c (plain C) without
 * new entry points here — net_init()/net_session_up() are hardwired to
 * ident's production host/port/dev_id/mqtt_pw/profile 2 and cannot be
 * reused for a not-yet-provisioned device. These functions never run in the
 * same power cycle as net_init(): main.c only reaches setup.c when
 * ident_load() has already failed, and setup_run() esp_restart()s on every
 * exit path, so there is no session-state overlap with the production
 * globals below (s_down_topic, the MQTT event handler registration, etc.)
 * to worry about.
 * --------------------------------------------------------------------- */

/* One-time bootstrap attach: WalterModem::begin() + event handler
 * registration + PDP context with the setup code's own `apn` (NULL/empty =
 * carrier default, same convention as net_init()) + single-attempt attach
 * wait (same F1-style cap net_init() uses). Deliberately skips
 * eDRX/PSM/voltage-monitor/TLS/mqttConfig — those are either production-only
 * concerns or handled by net_tls_profile_bootstrap()/net_bootstrap_connect()
 * below; there is no ident yet to configure any of net_init()'s ident-derived
 * pieces against. Call once, before net_tls_profile_bootstrap().
 * Power effect: same as net_init()'s attach phase — modem leaves reset and
 * attaches LTE-M, at whichever eDRX/PSM the modem defaults to (neither is
 * requested on this path): the bootstrap session is a few seconds long and
 * torn down (net_session_down() + esp_restart()) immediately after, so the
 * production sleep-mode paging settings do not matter here. */
bool net_bootstrap_attach(const char *apn);

/* One-time bootstrap MQTT connect: mqttConfig(client_id, client_id, password,
 * the bootstrap TLS profile) + mqttConnect(host, port). Also (re)points the
 * shared down-topic buffer at `down_topic` so the existing CONNECTED-event
 * auto-resubscribe (net.cpp's pager_mqtt_event_handler, the same mechanism
 * net_init()'s production session uses) subscribes to it — safe only
 * because this device never runs a bootstrap and a production session in
 * the same power cycle (see the module note above). Call after
 * net_bootstrap_attach() and net_tls_profile_bootstrap() both succeed; poll
 * net_get_mqtt_status().mqtt_connected afterward for the SUBSCRIBED edge,
 * same as the production path.
 * Power effect: one TLS handshake (~5 kB, VALIDATION_NONE, no CA round
 * trip) plus the RRC time it takes. */
bool net_bootstrap_connect(const char *client_id, const char *password, const char *host,
                           uint16_t port, const char *down_topic);

/* Writes `ca_pem` to modem NVRAM cert slot 12 (PAGER_TLS_CA_SLOT), the same
 * slot/call net_init() uses for the production CA — used by setup.c right
 * after a bootstrap fetch validates the bundle (docs/DEVICE_PLAN.md §3.2
 * step 4's "CA to slot 12"), unconditionally (no ca_hash short-circuit: this
 * runs at most once per device lifetime, unlike net_init()'s per-boot/per-F4
 * -recovery calls).
 * Power effect: one NVRAM write (flash wear on the modem's own storage, not
 * the ESP32's), no RRC. */
bool net_write_ca(const char *ca_pem);

/* TLS profile already configured by net_init(); this issues mqttConnect().
 * Call once after net_init() succeeds, and again (after F1/F3 backoff) any
 * time net_get_mqtt_status() reports the session down. Never call on a
 * timer (docs/PROTOCOL.md §6.1). Subscription happens from inside the
 * CONNECTED event handler (L3), never here.
 * Power effect: one TLS handshake, ~5 kB (PROTOCOL.md §7.2/§7.3), plus the
 * RRC time it takes (PENDING_HW for the current draw). */
bool net_session_up(void);

/* Explicit MQTT disconnect. Legal from the F3 "permanent failure" recovery
 * path (to force a clean modem-side teardown before the 300s steady
 * backoff) and from the v0.2 §2.3 connect watchdog (modes.c) when a connect
 * has produced neither CONNECTED nor DISCONNECTED within 60s - same "force
 * a clean modem-side teardown" reasoning, just a different trigger.
 * Power effect: one AT command, no RRC of its own. */
void net_session_down(void);

/* Publish `len` bytes of `buf` on `topic` at the given QoS. `buf` must be
 * mutable: WalterModem v1.5.0's mqttPublish() takes uint8_t*, not const
 * (L6). len > 640 (PROTOCOL.md §3.3) is refused.
 * Power effect: the RRC time for one publish if the modem was idle;
 * ~0 extra if it was already in an active RRC state (PENDING_HW). */
bool net_publish(const char *topic, char *buf, uint16_t len, uint8_t qos);

/* Same as net_publish(), typed for a binary (CBOR) payload instead of text.
 * WalterModem v1.5.0's mqttPublish() is confirmed binary-safe by reading the
 * vendor source (src/proto/WalterMQTT.cpp:97-103, src/WalterModem.cpp:
 * 2049-2068): the AT command line carries only the topic and the byte
 * count, and the payload is written with uart_write_bytes()/exactly
 * buf_size bytes after the modem's data prompt — no string handling or
 * escaping touches `buf`. len > 640 (PROTOCOL.md §3.3) is refused, same as
 * net_publish().
 * Power effect: the RRC time for one publish if the modem was idle; ~0
 * extra if it was already in an active RRC state (PENDING_HW). */
bool net_publish_raw(const char *topic, uint8_t *buf, uint16_t len, uint8_t qos);

/* Register the callback invoked once per inbound MQTT message, after net.c
 * has already bounds-checked it (§3.4/F6) and fetched it via mqttReceive().
 * Runs on the modem library's _eventProcessingTask (L4), not an ISR and not
 * the RX task — keep it short. `body` is not necessarily NUL-terminated
 * beyond `len` bytes; treat it as a fixed-length buffer.
 * PROTOCOL.md §2: the oversize check this callback sits behind is
 * topic-aware — `pager/boot/...` (setup.c, F3.5) gets the 4 kB bundle cap,
 * every other topic keeps the 640-byte envelope cap; `len` is bounded by
 * whichever cap applied to the topic the message arrived on. */
void net_set_msg_cb(void (*cb)(const char *topic, const char *body, uint16_t len));

/* Light-sleep the ESP32 for up to `ms` milliseconds, or until the button
 * (pins.h PAGER_PIN_BUTTON) wakes it early. This is net.c's own
 * esp_light_sleep_start() call, NOT WalterModem::sleep(t, true): the
 * library version is timer-only and would starve out the button's ext0
 * wake. It replicates the library's own RTS choreography by hand
 * (WalterModem.cpp:4413-4450): flow control off + RTS forced high before
 * sleep, uart_set_pin()/UART_HW_FLOWCTRL_CTS_RTS restored after.
 * Power effect: ESP32 draws the vendor-documented ~1 mA light-sleep floor
 * for up to `ms`; the modem is not touched and keeps paging on its own
 * eDRX cycle. */
void net_sleep(uint32_t ms);

/* checkComm() + getNetworkRegState(). Returns true if the modem answered
 * "AT" with "OK" and is still registered (HOME or ROAMING); false is the
 * F4 modem-not-responding signal.
 * Power effect: one AT round trip, no RRC. */
bool net_check(void);

/* F4 recovery ONLY: WalterModem::reset() (physical modem reset, destroys
 * the TLS+MQTT session) followed by a full re-run of net_init(). This is
 * the ONLY legal caller of reset()/softReset() outside of begin()'s own
 * cold-boot path (L5). The caller (modes.c) is responsible for the
 * 1-per-10-minutes rate limit — a wedged modem being reset in a loop is a
 * battery fire, per the spec.
 * Power effect: a full modem power cycle + re-attach, the single most
 * expensive recovery action in this design. Does NOT call net_session_up()
 * for you. */
bool net_recover_modem(void);

/* Best-effort wall clock, seeded once from the network (NITZ, via
 * getClock()) during net_init(). Returns false (and leaves *epoch_s
 * unchanged if non-NULL... actually sets it to 0) if no network time was
 * ever obtained; per PROTOCOL.md §3.5 the caller must then publish ts:0
 * forever rather than add an SNTP path. */
bool net_get_clock(int64_t *epoch_s);

/* Battery voltage in millivolts, via WalterModem::getVoltage() ->
 * AT+SQNVMON? (PROTOCOL.md §12 item 6: the GM02SP's own supply-rail
 * reading, believed but not yet hardware-confirmed to track the battery
 * cell directly rather than a fixed regulated rail — see §12's
 * unverified-assumptions table). net_init() must have already called
 * WalterModem::configVoltageMonitor() once; this just re-reads the current
 * value. Returns false (and leaves *batt_mv unchanged) if the AT command
 * fails — the caller must supply its own fallback, this function does not
 * cache a last-known-good value itself.
 * Power effect: one AT round trip, no RRC — same class as net_check(). */
bool net_get_battery_mv(int *batt_mv);

/* Signal strength in dBm, via WalterModem::getRSSI() -> AT+CSQ
 * (managed_components/dptechnics__walter-modem/src/WalterModem.h:4334;
 * implementation src/WalterModem.cpp:4478-4482, dBm conversion
 * src/WalterModem.cpp:2293). docs/DEVICE_PLAN.md §5.4 flagged which vendor
 * call (getRSSI() vs getSignalQuality()) exposes plain dBm as UNVERIFIED;
 * reading WalterModem.h settles it in favour of getRSSI() — getSignalQuality()
 * (AT+CESQ) returns RSRP/RSRQ instead, a different quantity. Returns false
 * (and leaves *dbm unchanged) if the AT command fails, or if the modem
 * reports "not known/not detectable" (raw AT+CSQ 99, which the vendor
 * converts to +85 dBm, outside its own documented [-113, -51] range) —
 * callers must not feed that into DEVICE_PLAN.md §5.4's dBm->bars table.
 * Power effect: one AT round trip, no RRC — same class as net_check(). */
bool net_get_rssi(int *dbm);

/* Read-only snapshot of MQTT connection state for modes.c's F1/F3 backoff
 * state machine. Does NOT clear disconnect_edge — call
 * net_ack_disconnect_edge() once you have actually acted on it. */
void net_get_mqtt_status(net_mqtt_status_t *out);

/* Acknowledge (clear) the disconnect edge latched in net_get_mqtt_status().
 * Exactly one call site in modes.c should call this, after it has decided
 * on a backoff/recovery action for the edge it just read. */
void net_ack_disconnect_edge(void);

/* True while net.cpp's MQTT event handler is inside an AT transaction
 * (mqttReceive()) or the app message callback. modes_run() MUST NOT
 * light-sleep while this is true: net_sleep() forces RTS high, and doing
 * that mid-response is the difference between PROTOCOL.md §8.3/M5 costing
 * latency and it costing the message. Racy by construction (a plain flag,
 * checked on the caller's task); it narrows the window from "every
 * incoming message" to a few microseconds, it does not close it. */
bool net_modem_busy(void);

/* Drain-and-reset delta counters, for folding into modes.c's RTC-resident
 * cumulative counters once per wake cycle. net.c only owns the
 * since-last-drain delta; modes.c owns the value that survives a reset. */
uint32_t net_take_memfull_delta(void);
uint32_t net_take_oversize_delta(void);

/* Granted eDRX value from the network (§6.3/§6.5, measurement M4), latched
 * from WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED. Copies an empty string
 * into `out` until the first eDRX URC arrives after attach. */
void net_get_granted_edrx(char *out, size_t out_size);

/* The device_id / MQTT client id in use (PROTOCOL.md §1: the two are
 * identical). Single source of truth for modes.c to build the /up and
 * /status topics; net.c already knows it for the /down subscription. */
const char *net_get_device_id(void);

/* Quick, network-attach-free SIM presence check for the pre-provisioning
 * boot screen (main.c) -- self-contained like net_bootstrap_attach(),
 * brings the modem up itself rather than depending on net_init()/ident.
 * WalterModem::getSIMCardIMSI() only needs NO_RF/FULL opstate, not a real
 * network attach, so this returns quickly regardless of signal. Power
 * effect: modem leaves reset, one AT round trip, no RRC. */
bool net_check_sim(void);

/* TEMPORARY diagnostic (main.c's `nettest` console command): attaches like
 * net_bootstrap_attach(), then dials a plain socket (no TLS at all, TCP if
 * udp==false else UDP) to host:port and polls for up to 10s for it to reach
 * WALTER_MODEM_SOCKET_STATE_OPENED. Added to isolate raw network/transport
 * reachability from the TLS/MQTT layer after net_bootstrap_connect() hung
 * for 30s with zero CONNECTED/DISCONNECTED event and no corresponding
 * connection attempt ever showing up in EMQX Cloud's own dashboard --
 * narrows "is this an APN/carrier-level block" from "is this a TLS/MQTT-
 * specific problem." The udp option was added after every TCP attempt (any
 * host, any port) hung identically while the vendor's own BlueCherry demo
 * (coap.bluecherry.io, UDP/DTLS) is known to work on this same SIM/kit --
 * narrows further to "TCP specifically" vs "all outbound traffic". Remove
 * once the broker-connect issue is root-caused. */
bool net_check_tcp(const char *host, uint16_t port, bool udp, bool tls);

/* TEMPORARY diagnostic (main.c's `mqtttest` console command): attaches like
 * net_bootstrap_attach(), then issues a real AT+SQNSMQTTCONNECT to host:port
 * over the VALIDATION_NONE bootstrap TLS profile with dummy credentials, and
 * waits up to 30 s for an MQTT event. Exists so a TLS server we control can
 * capture the ClientHello the modem's dedicated MQTT engine sends (SNI or
 * not), as opposed to nettest's socket-layer ClientHello. Remove together
 * with net_check_tcp(). */
bool net_check_mqtt(const char *host, uint16_t port, int tls_mode);

/* ---------------------------------------------------------------------
 * GNSS (docs/V02_DESIGN.md §5, docs/LOCATION_PLAN.md). Every power-effect
 * comment here is PENDING_HW/UNVERIFIED: nothing in this section has run on
 * a real Walter yet. loc.c is the only caller; it owns the whole
 * request/backoff/route policy and never calls WalterModem directly (this
 * header is the boundary, same rule every other net.h entry point follows).
 * --------------------------------------------------------------------- */

typedef enum {
    NET_GNSS_EVT_NONE = 0,  /* net_gnss_poll_event() found nothing new */
    NET_GNSS_EVT_FIX,       /* WMGNSSFixEvent status READY; fields below valid */
    NET_GNSS_EVT_NO_FIX,    /* status STOPPED_BY_USER/NO_RTC: attempt ended, no position */
    NET_GNSS_EVT_REFUSED,   /* status LTE_CONCURRENCY: modem refused a fix while attached */
} net_gnss_evt_t;

typedef struct {
    net_gnss_evt_t kind;
    double lat, lon;
    double confidence;  /* estimatedConfidence, metres; loc.c's threshold is <=100 */
    int64_t fix_ts;     /* unix seconds the fix was taken */
    uint8_t sat_count;
} net_gnss_event_t;

/* gnssConfig(): HIGH sensitivity, cold/warm-start acquisition, on-device
 * location. Persists across reboots per the vendor doc, so calling this more
 * than once (loc_init(), and again before every attempt) is a harmless
 * no-op AT command, not a re-provision. Power effect: one AT round trip, no
 * RRC, does not itself power the GNSS receiver. */
bool net_gnss_config(void);

/* gnssGetAssistanceStatus() for WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS.
 * *out_seconds_to_update is the vendor's own `timeToUpdate` (<=0 means due
 * now). Returns false if the AT command itself failed, which the caller
 * must treat as "assume due" (fail toward the slower/safer path, never
 * toward a fix attempt with stale ephemeris). Power effect: one AT round
 * trip, no RRC. */
bool net_gnss_assistance_due(int32_t *out_seconds_to_update);

/* gnssUpdateAssistance(REALTIME_EPHEMERIS): downloads over the still-attached
 * LTE session (must run before any detach — route 2). Blocking, bounded by
 * the modem's own command timeout; logs elapsed time as a stand-in for the
 * byte cost V02_DESIGN.md §5 asks to be measured (the vendor API reports
 * neither bytes nor a progress callback). Power effect: one LTE-attached
 * data transaction, a few kB (PENDING_HW, UNVERIFIED size — look for
 * "gnssUpdateAssistance" in the log). */
bool net_gnss_update_assistance(void);

/* gnssPerformAction(GET_SINGLE_FIX). True only means the modem accepted the
 * request ("OK") — NOT that a fix has arrived; the result comes later via
 * net_gnss_poll_event(). A synchronous false here is equivalent to a later
 * NET_GNSS_EVT_REFUSED and the caller should treat it the same way. Power
 * effect: starts the GNSS receiver; current draw continues until a result
 * event arrives or net_gnss_cancel() is called (PENDING_HW, UNVERIFIED). */
bool net_gnss_start_fix(void);

/* gnssPerformAction(CANCEL), best-effort, for when loc.c's own attempt
 * budget expires before a result event arrives. Power effect: stops the
 * GNSS receiver. */
void net_gnss_cancel(void);

/* Non-blocking: true and fills *out at most once per net_gnss_start_fix()
 * call, the first time net.cpp's GNSS event handler (WalterModem's own
 * _eventProcessingTask) has recorded a WALTER_MODEM_GNSS_EVENT_FIX. Mirrors
 * the msg-callback handoff net_set_msg_cb() documents: the event handler
 * itself only copies the struct and sets a flag; every log line and all
 * further modem API use happens here, on the caller's own task (loc.c's
 * loc_service(), from modes_run()). */
bool net_gnss_poll_event(net_gnss_event_t *out);

/* CFUN=4-equivalent (WALTER_MODEM_OPSTATE_NO_RF) — route 2's deliberate
 * radio-off window. Caller must already have called net_session_down()
 * (MQTT) first; modes.c's ordinary F1/F3/F4 recovery machinery must be
 * suppressed around this call and net_radio_on()/net_is_attached() below
 * (see modes_set_loc_suppress()). Power effect: LTE radio off; GNSS free of
 * LTE contention. */
bool net_radio_off(void);

/* Leaves the window: WALTER_MODEM_OPSTATE_FULL. Does not wait for
 * re-attach — poll net_is_attached() afterward, same non-blocking-per-call
 * discipline loc.c's whole state machine uses. Power effect: LTE-M
 * re-attach begins (PENDING_HW, same class as net_init()'s attach phase). */
bool net_radio_on(void);

/* getNetworkRegState() == HOME/ROAMING. Same cost class as net_check(): one
 * AT round trip, no RRC — safe to call once per loc_service() iteration
 * while polling for re-attach. */
bool net_is_attached(void);

/* Cell/tracking-area change trigger (V02_DESIGN.md §5 trigger 1). `cb` is
 * called from the network event handler (WalterModem's own event task — keep
 * it short, same rule as net_set_msg_cb()) with a short-lived "lac:ci" key
 * (net.cpp's own static buffer; copy it if the callback needs it afterward),
 * but ONLY when that key differs from the immediately previous one net.cpp
 * observed — net.cpp does this first, cheap de-duplication itself so loc.c's
 * own 10-minute debounce (loc_trigger_cell_change()) only has to reason
 * about genuine changes, not every REG_STATE_CHANGE URC. Requires
 * configCEREGReports(ENABLED_WITH_LOCATION), which net_init() now requests
 * (non-fatal if the modem rejects it — the trigger then just never fires,
 * which is this design's required fail-open behaviour). */
void net_set_cell_change_cb(void (*cb)(const char *cell_key));

/* Arms LIS3DH INT1 (pins.h PAGER_PIN_LIS3DH_INT1) as a second light-sleep
 * wake source alongside the button's ext0 (net_sleep()). Call once, from
 * accel.c, only after a successful WHO_AM_I probe — never call this if the
 * chip is absent, or an unwired/floating IO2 armed as a wake source would
 * wake the ESP32 on every light-sleep cycle for nothing. See net_sleep()'s
 * own comment for why this needs ext1 (not a second ext0) and which level
 * mode it uses. Power effect: none by itself; adds an early-wake path to
 * the existing ~1 mA light-sleep floor. */
void net_enable_accel_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_H */

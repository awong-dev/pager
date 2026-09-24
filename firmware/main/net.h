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

/* The MQTT session transport (docs/WIFI_DESIGN.md §1, docs/WIFI_TASKS.md W4).
 * Declared here, not in net_xport.h, so this header stays includable from
 * modes.c (a plain C file) without pulling in the ops-vtable header that
 * only net.cpp's dispatcher and the xport_*.c(pp) implementations need.
 * Today (W4) NET_XPORT_LTE is the only implementation that exists; WIFI is a
 * named placeholder for W5. */
typedef enum {
    NET_XPORT_LTE = 0,
    NET_XPORT_WIFI,
} net_xport_t;

typedef struct {
    bool                 mqtt_connected;   /* true from SUBSCRIBED(rc==0) until the next DISCONNECTED */
    bool                 disconnect_edge;  /* set once per fresh loss; caller must ack it */
    int                  last_rc;          /* raw WMMQTTConnRC from the event that set the edge */
    net_mqtt_rc_class_t  last_class;       /* classification of last_rc, see F3 */
    bool                 session_restart_edge; /* v0.2 §9.4 step 4: set once when a modem-initiated
                                                 * silent resume (§9.1 item 2) has just been repaired
                                                 * by net_service_session()'s raw re-SUBSCRIBE; caller
                                                 * must ack it via net_ack_session_restart_edge(). Never
                                                 * set together with a mqtt_connected false->true edge
                                                 * (a resume never clears mqtt_connected in the first
                                                 * place) -- modes.c treats the two as equivalent triggers
                                                 * for the same re-announce block regardless. */
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

/* v0.2 §4.4 (CA trust: two-phase apply). The scratch slot production
 * profile 2 must never be left pointed at after a failed apply (this design
 * rule): 12 is NET_TLS_CA_SLOT (the same value PAGER_TLS_CA_SLOT names
 * internally), 13 is NET_TLS_CA_SCRATCH_SLOT. */
#define NET_TLS_CA_SLOT 12
#define NET_TLS_CA_SCRATCH_SLOT 13

/* Writes `ca_pem` to modem NVRAM cert slot `slot` — generalises net_write_ca()
 * (which is really just this called with slot=NET_TLS_CA_SLOT) so catrust.c's
 * two-phase apply can write a candidate CA to the scratch slot
 * (NET_TLS_CA_SCRATCH_SLOT) without disturbing whatever is already in slot
 * 12, and reuse this same call for slot 12 on commit (no modem-side "copy"
 * operation exists or is needed: the caller already holds the plaintext PEM
 * from the fetch).
 * Power effect: one NVRAM write, no RRC. */
bool net_write_ca_slot(uint8_t slot, const char *ca_pem);

/* v0.2 §4.2/§4.4: reconfigures MQTT profile 2 (PAGER_TLS_PROFILE_ID) to name
 * `ca_slot` with validation on/off — ALWAYS naming a slot, never omitting
 * one (GOTCHAS.md's rule: an unnamed slot makes the MQTT engine send a
 * plaintext CONNECT). Used for: the daily/cold-boot revalidation attempt
 * while `broken` (ca_slot=NET_TLS_CA_SLOT), the fallback into `broken`
 * itself (ca_slot=NET_TLS_CA_SLOT, validated=false), the two-phase apply's
 * scratch trial (ca_slot=NET_TLS_CA_SCRATCH_SLOT, validated=true) and its
 * rollback (ca_slot=NET_TLS_CA_SLOT, back to whatever validation state
 * applied before the trial). Call before net_session_up() (or after
 * net_session_down() and before the next net_session_up()) — never touches
 * a live session itself.
 * Power effect: one AT command, no RRC of its own. */
bool net_tls_configure(uint8_t ca_slot, bool validated);

/* v0.2 §4.4 (CA fetch): a dedicated socket (distinct from nettest's) on TLS
 * profile 3 (validation off, NET_TLS_CA_SLOT still named — the same
 * "every TLS profile names a slot, even with validation off" rule applies
 * here too, even though nothing validates against it: trust comes from
 * cafetch.c's own SHA-256 check, not from this transport). Opens the socket
 * and dials `host:port` — BLOCKING (the AT+SQNSD dial, including the TLS
 * handshake, completes before this returns; "OK means dialled", same
 * contract net_check_tcp() already documents). Call from a task that can
 * afford a few seconds — never from modes_run()'s own task directly (see
 * cafetch.h's module comment for how catrust.c avoids that: the *fetch's*
 * own byte-by-byte progress is polled non-blockingly via
 * net_ca_fetch_poll() below, but the initial dial is not).
 * Power effect: one TLS handshake (~5 kB, VALIDATION_NONE, no CA round trip)
 * plus the RRC time it takes — same class as net_session_up(). */
bool net_ca_fetch_open(const char *host, uint16_t port);

/* Sends `len` bytes (the HTTP GET request line + headers) on the cafetch
 * socket. Power effect: the RRC time for one send if the modem was idle. */
bool net_ca_fetch_send(const uint8_t *buf, uint16_t len);

/* Non-blocking poll for ONE event on the cafetch socket since the last call:
 * either a `+SQNSRING` URC (issues one bounded socketReceive() AT round
 * trip, <=1500 bytes, and copies whatever was actually delivered into
 * `buf`/`*out_len` — bounded by the modem's own claim, the bytes actually
 * present, and `cap`, exactly like the vendor receive-bounds patch
 * (PATCHES.md 1.2) already guarantees for every socketReceive() caller) or
 * the socket closing (`*out_closed`, e.g. the server's `Connection: close`).
 * Returns false (`*out_len`/`*out_closed` untouched) if neither has happened
 * since the last poll — call this every cafetch_poll() iteration (which
 * itself is called every catrust_service() iteration), like
 * net_gnss_poll_event(). Power effect: none when it returns false; one AT
 * round trip (socketReceive()) when it returns a RING. */
bool net_ca_fetch_poll(uint8_t *buf, size_t cap, uint16_t *out_len, bool *out_closed);

/* Tears down the cafetch socket (best-effort AT+SQNSH). Power effect: one AT
 * command, no RRC of its own. */
void net_ca_fetch_close(void);

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
 * backoff), from the v0.2 §2.3 connect watchdog (modes.c) when a connect
 * has produced neither CONNECTED nor DISCONNECTED within 60s, and (v0.2 M3,
 * 22 Sep evening) from net_service_session()'s own host-detected-dead
 * branches (the liveness-ping-no-SUBACK case and the M1 connect-timeout
 * case) -- all four are "force a clean modem-side teardown", just different
 * triggers. Also clears the M1/M2 connect-in-flight state (net_connect_in_flight()).
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

/* 23 Sep display-corruption fix (publish_quiet.h's own module comment):
 * blocks (vTaskDelay, never a tight loop) while a pager-originated publish
 * issued via net_publish()/net_publish_raw() is outstanding or has
 * completed less than PUBLISH_QUIET_WINDOW_US ago, up to `max_wait_ms`.
 * Returns the actual ms waited (0 if nothing was in flight). disp.c's
 * pre-refresh gate hook (ui.c's strong disp_pre_write_gate_hook()) is the
 * only caller — see disp.h's own comment on that layering seam.
 * Power effect: none of its own; it only delays a refresh that was already
 * about to happen, up to max_wait_ms. */
uint32_t net_publish_quiet_wait_ms(uint32_t max_wait_ms);

/* RCA_SLEEP_PUBLISH.md §3 instrumentation: the vendored walter-modem
 * component's four free-running "orphaned prompt" counters (PATCHES.md
 * 1.12), read-only here. modes.c's sleeptest report prints these so a
 * bench run's evidence survives the USB-dead light-sleep window (the counters
 * themselves live in RAM inside the component and reset only on a full
 * reset/power cycle, same as the rest of RAM). Power effect: none -- four
 * plain reads. */
typedef struct {
    uint32_t datatx_retx;    /* patch 1.12: DATA_TX_WAIT timeouts answered by
                              * sending the payload instead of the AT command line */
    uint32_t prompt_orphan;  /* patch 1.11's bare "> " prompt case matched --
                              * the discriminator for RCA §2 */
    uint32_t buf_drop_queue; /* a fully-parsed RX buffer dropped, 8-slot queue full */
    uint32_t buf_drop_pool;  /* an RX buffer allocation failed, 8-buffer pool exhausted */
    /* S3 (patch 1.13, docs/RCA_SLEEP_URC.md §5 fix 3-4): attribution for the
     * two 30s stalls fix 1's own arithmetic could not tell apart -- see
     * WalterDefines.h's own comment on walter_modem_pager_counters_t. */
    uint32_t prompt_handled;        /* the "> " prompt handler actually wrote a payload */
    uint32_t payload_bytes_written; /* total bytes actually written by that write, both paths */
    uint32_t txdone_timeouts;       /* uart_wait_tx_done() did not return ESP_OK */
    char stall_cmd[25];             /* first 24 chars of the most recently observed slow command's
                                      * AT line, or "" if none has taken >= 5s yet this boot */
    uint32_t stall_elapsed_ms;      /* how long that attempt had run when last sampled */
    int32_t stall_cts_level;        /* CTS pin level at that sample, -1 if unavailable */
    uint32_t stall_tx_ring_bytes;   /* UART TX ring free bytes at that sample (see
                                      * WalterDefines.h: reads 0 on this UART's 0-byte TX ring) */
} net_pager_counters_t;
net_pager_counters_t net_get_pager_counters(void);

/* RCA_SLEEP_PUBLISH.md §3 instrumentation: a 12-entry ring of the most recent
 * net_publish()/net_publish_raw() attempts over the LTE transport (topic
 * tail, byte length, issue time, outcome and how long the blocking AT round
 * trip took), for the same sleeptest report. Always safe to call regardless
 * of the active transport (net_xport_active()) -- empty if nothing has
 * published over LTE yet this boot. Entries are returned oldest-first, up to
 * `cap` of them; the return value is the number actually written (<= cap and
 * <= however many have occurred, whichever is smaller). Power effect: none
 * -- a RAM copy, no AT traffic. */
typedef enum {
    NET_PUBLISH_RING_OK = 0,
    NET_PUBLISH_RING_TIMEOUT,
    NET_PUBLISH_RING_ERROR,
} net_publish_outcome_t;

typedef struct {
    int64_t issued_us;               /* esp_timer_get_time() at issue */
    char topic_tail[8];              /* last up to 7 chars of the topic + NUL */
    uint16_t len;
    net_publish_outcome_t outcome;
    uint32_t elapsed_ms;             /* wall time the blocking mqttPublish() call took */
} net_publish_ring_entry_t;
/* Capacity of the ring xport_lte.cpp maintains and net_get_publish_ring()
 * reads -- shared here so callers can size a snapshot array without a
 * separate xport_lte.cpp-only constant. */
#define NET_PUBLISH_RING_MAX 12
uint32_t net_get_publish_ring(net_publish_ring_entry_t *out, uint32_t cap);

/* S2 (docs/SLEEP_URC_DESIGN.md §6, "SUBACK collision"): count of how often a
 * liveness ping's first re-SUBSCRIBE went unanswered for 30s (whether the
 * session was then proven alive by a /down message, rescued by a second
 * re-SUBSCRIBE, or -- if that second one also went unanswered -- declared
 * dead). Zero across a whole sleeptest window is the expected/healthy case;
 * a non-zero count with zero `MQTT session LOST` lines is this fix working
 * as intended. Power effect: none -- one plain read. */
uint32_t net_get_resub_swallowed_count(void);

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

/* S1 (docs/SLEEP_URC_DESIGN.md §3(a)/§5): per-wake URC drain probe. Issues
 * one asynchronous, fire-and-forget WalterModem::checkComm(NULL, cb, NULL) --
 * one bare "AT", no retries, no net_check() registration semantics, no
 * watchdog interaction -- so a URC the modem is holding for lack of a
 * command to release it on is freed inside the wake's existing yield. Call
 * on every wake, in both ACTIVE and SLEEP mode, as the first statement after
 * net_sleep() returns (flow control is restored by then) -- NOT under the
 * `mode == SLEEP` / `loc_suppress` / `ca_apply_suppress` / `coverage_owns_
 * radio` gates run_modem_health_check() uses: a bare "AT" says nothing about
 * registration and resets nothing, so none of those reasons to skip the
 * health check apply here.
 * Single-slot in-flight guard: returns false without issuing anything if a
 * previous probe is still outstanding, if net_publish_in_flight() (a modem
 * parked at a "> " prompt would consume this probe's "AT\r\n" as payload --
 * RCA_SLEEP_PUBLISH.md §1), on the WiFi transport, or if the modem is not
 * begun / is mid-reset. An outstanding probe not answered within 3 further
 * calls to this function (3 wake intervals) is declared stuck (counted,
 * flag cleared so probing resumes) -- see net_get_probe_counters(). This
 * function never touches rate_limited_modem_recover() itself; a caller that
 * wants the existing F4 escalation after repeated stuck probes must drive
 * it from net_get_probe_counters().stuck.
 * Power effect: none of its own -- the answer (<10ms) lands inside the
 * existing post-wake yield; +0 ms of window, +0 air bytes. */
bool net_urc_probe(void);

/* net_urc_probe()'s counters since boot, for the sleeptest report:
 * issued    - probes actually queued;
 * answered  - queued probes whose "OK" came back;
 * stuck     - probes given up on after 3 wake intervals unanswered;
 * noqueue   - checkComm() could not queue the probe at all (8-slot queue/
 *             pool full -- WalterModem.h:127,3501), counted instead of
 *             issued, no retry.
 * Power effect: none -- four plain reads. */
typedef struct {
    uint32_t issued;
    uint32_t answered;
    uint32_t stuck;
    uint32_t noqueue;
} net_probe_counters_t;
net_probe_counters_t net_get_probe_counters(void);

/* RCA_SLEEP_URC.md fix 1's discriminator: bytes currently sitting in the
 * modem UART's RX ring (uart_get_buffered_data_len()), for modes.c to sample
 * 50 ms after each wake -- near-zero on an ordinary wake, a burst on the
 * wake after a page once a held URC is actually being released. Returns 0
 * on a driver error, same as an empty ring (this is a diagnostic, not a
 * correctness signal). Power effect: none -- one UART driver software
 * counter read, no AT traffic. */
uint32_t net_uart_rx_buffered_bytes(void);

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

/* v0.2 §9.4: the idle-uplink liveness ping / silent-resume repair. Call once
 * per wake-and-drain loop iteration from modes.c's own task (never from an
 * event callback), guarded by the same three suppressions the reconnect path
 * already honours (coverage duty cycle, location route 2, a CA-apply trial).
 * Sends a raw AT+SQNSMQTTSUBSCRIBE to the down-topic (via WalterModem::sendCmd(),
 * the same path net_debug_at() uses) every PAGER_MQTT_PING_S seconds of
 * uplink silence, or immediately after a modem-initiated resume (§9.1 item 2)
 * -- the vendor's mqttSubscribe() would silently no-op that resubscribe
 * (WalterMQTT.cpp:120-123, "Topic already in use") since mqttConnect() is the
 * only thing that frees its local topic table and a modem-initiated resume
 * never calls it. Also the early-death detector §9.1 lacked: no SUBACK within
 * 30s marks the session dead (disconnect_edge, NET_MQTT_RC_TRANSIENT) so the
 * ordinary F1/F3 backoff + net_session_up() path runs, instead of waiting out
 * the modem's own ~6 minute silent-resume window.
 *
 * v0.2 M1 (22 Sep outage): also checks a second, independent bound -- a
 * connect issued by net_session_up() with neither CONNECTED nor SUBSCRIBED
 * seen within 30s (net_connect_guard.h's NET_CONNECT_TIMEOUT_US) is declared
 * dead the same way. Both this and the no-SUBACK case above call
 * net_session_down() themselves (v0.2 M3) before setting disconnect_edge, so
 * the modem's MQTT client is never left connected out from under a host
 * verdict that it is dead -- see net_session_down()'s own updated doc
 * comment.
 * Power effect: nothing when idle and under the ping interval; otherwise one
 * AT round trip (the RRC time for one subscribe if the modem was idle, ~0
 * extra if it was already active) at most once per PAGER_MQTT_PING_S, or (on
 * either dead-detection edge) one AT+SQNSMQTTDISCONNECT.
 *
 * docs/WIFI_TASKS.md W4: modes.c now calls this unconditionally, every wake
 * cycle; the three suppressions (coverage duty cycle, location route 2, a
 * CA-apply trial) that used to gate the call site instead gate the LTE
 * transport internally via net_set_lte_suppressed() below -- they are about
 * the *modem*, so they must not also silence a future WiFi transport's
 * service tick. */
void net_service_session(void);

/* docs/WIFI_TASKS.md W4: the transport seam's read side. Always
 * NET_XPORT_LTE until W5 adds a second transport; exists now so a future
 * `/status` `xport` field (docs/WIFI_DESIGN.md §6) has something to read. */
net_xport_t net_xport_active(void);

/* docs/WIFI_TASKS.md W5: the only writer of the transport seam's active
 * transport (see net.cpp's own doc comment on this function for the full
 * teardown/wait/bring-up contract). A no-op if `to` is already active.
 * Callers today: main.c's debug-build `wifi on`/`wifi off` console commands
 * only -- no automatic policy calls this in phase 1 (docs/WIFI_DESIGN.md
 * §3: "nothing turns it on by itself"). */
void net_xport_switch(net_xport_t to);

/* docs/WIFI_TASKS.md W4: the three suppressions that used to gate
 * modes.c's net_service_session() call site (coverage duty cycle, location
 * route 2, a CA-apply trial -- all about the *modem*) now gate the LTE
 * transport's service tick from inside net_service_session() instead, so a
 * future WiFi transport's tick is never silenced by them. modes.c calls this
 * with the same boolean expression it used to guard the call site with,
 * every wake cycle, before calling net_service_session() unconditionally.
 * Power effect: none of its own -- it only decides whether the next
 * net_service_session() call is allowed to touch the modem. */
void net_set_lte_suppressed(bool suppressed);

/* Acknowledge (clear) the session_restart_edge latched in
 * net_get_mqtt_status(). Exactly one call site in modes.c should call this,
 * the same re-announce block that already handles the ordinary
 * mqtt_connected false->true edge (docs/V02_DESIGN.md §9.4 step 4/§9.5). */
void net_ack_session_restart_edge(void);

/* True while net.cpp's MQTT event handler is inside an AT transaction
 * (mqttReceive()) or the app message callback. modes_run() MUST NOT
 * light-sleep while this is true: net_sleep() forces RTS high, and doing
 * that mid-response is the difference between PROTOCOL.md §8.3/M5 costing
 * latency and it costing the message. Racy by construction (a plain flag,
 * checked on the caller's task); it narrows the window from "every
 * incoming message" to a few microseconds, it does not close it. */
bool net_modem_busy(void);

/* v0.2 M1/M2 (22 Sep outage): true from the moment net_session_up() queues
 * mqttConnect() until CONNECTED/SUBSCRIBED arrives, net_session_down() runs,
 * or M1's 30s connect timeout fires (net_service_session()) -- a distinct
 * accessor from net_modem_busy() rather than folded into it, because the two
 * guard different races: net_modem_busy() is the UART/RTS interlock against
 * an in-progress AT *response* the event handler is mid-processing (a few ms
 * to a couple of seconds); this is "a CONNECT is outstanding" for as long as
 * the TLS handshake + CONNACK can plausibly still be coming (up to 30s).
 * modes.c's skip_sleep OR's this in (M2's own fix for the outage: the ESP32
 * used to light-sleep 110ms after issuing a connect, deasserting RTS while
 * the handshake was still in flight, which is exactly how the CONNECTED
 * event got lost). Deliberately NOT folded into pump_blocked: msg_pump()'s
 * gating exists for the mqttReceive() AT-response race (net_modem_busy()'s
 * own doc comment above), not for holding the CPU awake during a connect --
 * no message can usefully be published while the session is not up yet, and
 * even a stale publish attempt just fails cheaply rather than corrupting a
 * different modem-side transaction. */
bool net_connect_in_flight(void);

/* 23 Sep release-build fix, corrected by docs/RCA_SLEEP_PUBLISH.md (23 Sep):
 * true from the moment net_publish()/net_publish_raw() issues a publish
 * until the matching WALTER_MODEM_MQTT_EVENT_PUBLISHED event runs
 * (publish_quiet_gate_done()), bounded by PUBLISH_SLEEP_HOLD_MAX_US (15s,
 * see publish_quiet.h) so a lost PUBLISHED URC cannot pin the device awake
 * indefinitely -- same shape as net_connect_in_flight() just above, for the
 * publish window instead of the connect window.
 *
 * What this hold actually does (RCA §4 item 3, §1's original-mechanism
 * table): net_publish()/net_publish_raw()'s underlying mqttPublish() call is
 * SYNCHRONOUS -- it blocks the calling task until the whole AT round trip
 * completes -- so a publish issued from the *modes* task (the `/up` ack via
 * msg_pump(), `/status` heartbeats, `/loc`, sms_log, book) already cannot
 * reach net_sleep() while this hold would apply; RCA §1 refutes "our own
 * light sleep during our own publish" as that bug's mechanism. This hold's
 * real, currently-unobserved effect is closing the *event-task* publish
 * window instead -- `/status` on the incoming-page mode edge and setup.c's
 * `/up` setup ack, both issued from WalterModem's own _eventProcessingTask,
 * which net_sleep() (running on the modes task, priority 1 against the event
 * task's priority 4) previously had no guard against at all outside of
 * s_handler_busy's incidental MESSAGE/CONNECTED coverage.
 *
 * The observed bench corruption (a 44-byte publish's own retried AT command
 * line consumed as its payload, tripping the relay's bad-sig check) is NOT
 * this race: it is the "> " data prompt being orphaned inside the parser
 * (RCA §2) followed by the old unconditional command-line retry on timeout
 * (RCA §4 item 2) -- fixed by patches 1.11 and 1.12 (PATCHES.md), not by
 * this gate. modes.c's skip_sleep ORs this in; deliberately NOT folded into
 * pump_blocked, same reasoning as net_connect_in_flight()'s own doc comment
 * above. */
bool net_publish_in_flight(void);

/* v0.2 M3 (22 Sep evening): true once net_session_up() has failed
 * NET_SESSION_UP_FAIL_ESCALATE (3, net_connect_guard.h) times in a row at the
 * mqttConfig()/mqttConnect() step -- modes.c's retry branch escalates to
 * rate_limited_modem_recover() when this is true instead of another ordinary
 * backoff step, mirroring the existing 60s connect-watchdog's own 3x
 * escalation. Resets to false the moment a connect is successfully queued
 * again (net_connect_guard_issued()'s own contract) or after
 * net_recover_modem() runs. */
bool net_connect_fail_streak_maxed(void);

/* Drain-and-reset delta counters, for folding into modes.c's RTC-resident
 * cumulative counters once per wake cycle. net.c only owns the
 * since-last-drain delta; modes.c owns the value that survives a reset. */
uint32_t net_take_memfull_delta(void);
uint32_t net_take_oversize_delta(void);

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

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
/* TEMPORARY diagnostic (main.c's `nettest` console command), debug build
 * only (PAGER_DEBUG_NO_LIGHT_SLEEP) -- docs/ROADMAP.md's "temporary
 * diagnostics" no longer ship in the release binary. Attaches like
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

/* TEMPORARY diagnostic: plain TCP like net_check_tcp(), but sends a valid HTTP/1.0 GET padded to
 * `bytes` and then waits 12 s so the AT trace shows whether a reply rings (+SQNSRING). */
bool net_check_tcp_sized(const char *host, uint16_t port, size_t bytes);
#endif /* PAGER_DEBUG_NO_LIGHT_SLEEP */

/* Debug build only: send one raw AT command; the reply shows in the AT trace. */
bool net_debug_at(const char *cmd);

/* Coverage tracking (net.cpp's block comment above net_bringup()).
 * net_take_registered_edge(): true once after the pager goes from not
 * registered to registered. net_unregistered_for_s(): seconds without a
 * network, 0 when registered. */
bool net_take_registered_edge(void);
uint32_t net_unregistered_for_s(void);

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
/* TEMPORARY diagnostic (main.c's `mqtttest` console command), debug build
 * only (PAGER_DEBUG_NO_LIGHT_SLEEP) -- same as net_check_tcp() above.
 * Attaches like net_bootstrap_attach(), then issues a real
 * AT+SQNSMQTTCONNECT to host:port over the VALIDATION_NONE bootstrap TLS
 * profile with dummy credentials, and waits up to 30 s for an MQTT event.
 * Exists so a TLS server we control can capture the ClientHello the modem's
 * dedicated MQTT engine sends (SNI or not), as opposed to nettest's
 * socket-layer ClientHello. Remove together with net_check_tcp(). */
bool net_check_mqtt(const char *host, uint16_t port, int tls_mode);
#endif /* PAGER_DEBUG_NO_LIGHT_SLEEP */

/* ---------------------------------------------------------------------
 * GNSS (docs/V02_DESIGN.md §5, docs/V02_DESIGN.md §5). Every power-effect
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

/* Serving-cell snapshot for PROTOCOL.md §13.2's `/loc` `cell` sub-map (this
 * task). Deliberately the SERVING network's own MCC/MNC (AT+SQNMONI, via
 * WalterModem::getCellInformation()), NOT the SIM's home PLMN/IMSI: on the
 * bench pager the SIM's IMSI starts 310280 but the serving network is
 * 310/410, and the relay's cellgeo lookup only works with the serving pair
 * (loc.c/main's own note, this task). */
typedef struct {
    bool valid;       /* false: never read successfully this power session -- omit `cell` entirely */
    char mcc[4];       /* 3 ASCII digits + NUL, PROTOCOL.md §13.2 */
    char mnc[4];       /* 2-3 ASCII digits + NUL, leading zeros kept */
    uint16_t tac;      /* 0..65535 */
    uint32_t ci;       /* 28-bit E-UTRAN cell id, 0..268435455 */
    bool have_rsrp;
    int rsrp;          /* dBm, -156..-30 when have_rsrp */
} net_cell_info_t;

/* Reads/returns the cached serving-cell snapshot, refreshing it with ONE
 * AT+SQNMONI round trip only when the cache is stale (never fetched yet this
 * power session, or a genuine cell change was observed via
 * net_set_cell_change_cb()'s own de-duplicated URC) -- never on every call,
 * so loc.c sharing one cached snapshot across several queued requesters
 * (loc.h's own queue) or across a single /loc decision costs at most one AT
 * round trip, not one per requester. Returns `out->valid` (also the return
 * value): false if the cell has never been read successfully yet, in which
 * case the caller must omit `cell` from the wire entirely (PROTOCOL.md
 * §13.2: "if the cell cannot be read, send the answer without it") -- on a
 * refresh failure with an earlier good reading cached, the stale reading is
 * returned rather than dropped (logged either way).
 *
 * MNC digit count (2 vs 3): read from the raw `+SQNMONI` response width via
 * the vendored library's `ncDigits` field (PATCHES.md 1.9) when available
 * (`ncDigits` 2 or 3); otherwise (patch absent, or the field was
 * unparseable) falls back to a small NANP-MCC table (302, 310-316, 330,
 * 332 -> 3 digits, the ITU/3GPP convention behind "US networks are 3
 * digits" -- everything else assumed 2 digits, UNVERIFIED outside NANP, see
 * net.cpp's own `is_nanp_mcc()` comment).
 *
 * Called only from loc.c's own task (never from an event callback -- this
 * file's own "modem calls only through this facade, never from a callback"
 * rule). Power effect: 0 or 1 AT round trip, no RRC of its own -- same class
 * as net_check()/net_get_rssi(). */
bool net_get_cell_info(net_cell_info_t *out);

/* Enables or disables LIS3DH INT1 (pins.h PAGER_PIN_LIS3DH_INT1) as a
 * second light-sleep wake source alongside the button's ext0
 * (net_sleep()). A1 (docs/DEVICE_NEXT_TASKS.md): evaluated fresh on every
 * net_sleep() call rather than latched once, so accel.c can disarm it for
 * a refractory window after each edge it reports to the motion classifier
 * -- CTRL_REG5's LIR_INT1 latch plus ext1's ANY_HIGH mode would otherwise
 * end light sleep up to ~10x/s (10 Hz ODR) while the pager is being
 * carried, for no benefit to a classifier that only needs two edges >=60s
 * apart. Call with `true` only after a successful WHO_AM_I probe -- an
 * unwired/floating IO2 armed as a wake source would wake the ESP32 on
 * every light-sleep cycle for nothing. See net_sleep()'s own comment for
 * why this needs ext1 (not a second ext0) and which level mode it uses.
 * Power effect: none by itself; adds/removes an early-wake path to/from
 * the existing ~1 mA light-sleep floor. */
void net_set_accel_wake(bool on);

/* Back-compat wrapper for net_set_accel_wake(true) -- accel_init()'s own
 * "the chip just answered WHO_AM_I, arm the wake source" call. */
void net_enable_accel_wake(void);

/* Count of esp_light_sleep_start() returns (net_sleep()) whose wakeup
 * cause was ESP_SLEEP_WAKEUP_EXT1 (the LIS3DH motion pin), since boot.
 * A1's before/after storm-guard measurement counter: A2's `acceltest`
 * prints it, A4 measures with it on the bench (refr 0 vs. refr 20, walk
 * 60s each, expect roughly two orders of magnitude fewer with the guard
 * on). Power effect: none -- read-only counter. */
uint32_t net_get_ext1_wakes(void);

/* ---------------------------------------------------------------------
 * SMS (docs/V02_DESIGN.md §6). main/sms.c is the only caller; it owns the
 * allow-list/audit/encoding policy and never calls WalterModem directly
 * (this header is the boundary, same rule every other net.h entry point
 * follows). Every power-effect comment here is PENDING_HW/UNVERIFIED: SMS
 * on the production SIM has not been tested on real hardware while writing
 * this (V02_DESIGN.md §6's own flag) -- `smstest`/`smslist` exist to find
 * out.
 * --------------------------------------------------------------------- */

typedef struct {
    bool used_ira;      /* true: AT+CSCS="IRA" was accepted; false: fell back to AT+CSCS="GSM" */
    int storage_used;   /* AT+CPMS's own <usedr> (read storage), from its SET-command response;
                         * -1 if unknown/unavailable (coordinator fix, smaller item a) */
    int storage_total;  /* AT+CPMS's own <totalr>; -1 if unknown/unavailable */
} net_sms_config_result_t;

/* One-time text-mode SMS setup (WalterModem::smsConfig(): AT+CMGF/AT+CSCS
 * ("IRA", falling back to "GSM")/AT+CSDH/AT+CSMP/AT+CNMI/AT+CPMS, see the
 * vendor patch's own PATCHES.md entry). Call once from sms_init(); a false
 * return means SMS is unsupported/unavailable this boot (docs/V02_DESIGN.md
 * §0: log once at INFO, disable the feature, never touch paging) -- sms.c,
 * not this file, decides what to do with that. `out` (nullable) reports
 * which charset actually won and the AT+CPMS-reported "ME" storage usage/
 * capacity (both -1 if that response did not include them) -- sms.c uses
 * the latter to bound its boot-drain scan instead of a hardcoded guess.
 * Power effect: up to six AT round trips, no RRC of their own. */
bool net_sms_config(net_sms_config_result_t *out);

/* Sends one SMS via WalterModem::smsSend(). `text` is either plain text
 * (7-bit path, `use_ucs2` false -- sms.c's own sms_charset_mode_t decides
 * which characters are eligible, see sms.h's own module comment) or a hex
 * string of big-endian UTF-16 code units (`use_ucs2` true) -- sms.c's own
 * encoding decision; this facade never touches character encoding itself.
 * Power effect: an AT+CSCS/AT+CSMP toggle (two round trips) either side of
 * the send ONLY when `use_ucs2` (a plain 7-bit send costs one AT+CMGS
 * transaction only, RRC-active for its duration, PENDING_HW/UNVERIFIED). */
bool net_sms_send(const char *number, const char *text, bool use_ucs2);

typedef struct {
    bool valid;         /* false: the index was empty/nonexistent (still "OK", not a command failure) */
    char sender[32];    /* raw <oa>, ASCII digits, not charset-decoded (net.cpp's own module comment) */
    char timestamp[32]; /* raw <scts>, ASCII */
    int dcs;            /* 3GPP TS 23.038 §4 data coding scheme, or -1 if AT+CSDH=1's extra
                         * +CMGR fields were absent from this response (coordinator fix #2:
                         * sms.c uses this to decide the body's encoding instead of guessing) */
    char body[281];     /* GSM-7 text, or UCS-2 hex -- sms.c's decoder tells them apart via `dcs` */
    uint16_t body_len;
} net_sms_read_t;

/* Reads one SMS record (WalterModem::smsRead(), AT+CMGR, text mode).
 * Returns false only on an outright command failure (ERROR/+CMS ERROR) --
 * an empty/nonexistent index is reported via `out->valid == false`, not a
 * false return (mirrors the vendor command's own "OK but nothing there" vs.
 * "ERROR" distinction, UNVERIFIED which this modem actually does). Power
 * effect: one AT+CMGR round trip. */
bool net_sms_read(int index, net_sms_read_t *out);

/* Deletes one SMS record (WalterModem::smsDelete(), AT+CMGD). Power effect:
 * one AT round trip. */
bool net_sms_delete(int index);

typedef struct {
    uint16_t index;
    char mem[8]; /* storage name the modem reported, e.g. "ME" */
} net_sms_event_t;

/* Non-blocking: true and fills *out at most once per `+CMTI` URC observed
 * since the last call -- same single-flag event handoff pattern as
 * net_gnss_poll_event() (the event handler itself only copies the struct
 * and sets a flag; every subsequent modem call -- net_sms_read()/
 * net_sms_delete() -- happens here, on the caller's own task, sms.c's
 * sms_service() from modes_run(), never from the event handler itself).
 * Call every sms_service() iteration. Power effect: none when it returns
 * false. */
bool net_sms_poll_event(net_sms_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NET_H */

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

/* TLS profile already configured by net_init(); this issues mqttConnect().
 * Call once after net_init() succeeds, and again (after F1/F3 backoff) any
 * time net_get_mqtt_status() reports the session down. Never call on a
 * timer (docs/PROTOCOL.md §6.1). Subscription happens from inside the
 * CONNECTED event handler (L3), never here.
 * Power effect: one TLS handshake, ~5 kB (PROTOCOL.md §7.2/§7.3), plus the
 * RRC time it takes (PENDING_HW for the current draw). */
bool net_session_up(void);

/* Explicit MQTT disconnect. ONLY legal from the F3 "permanent failure"
 * recovery path, to force a clean modem-side teardown before the 300s
 * steady backoff. Power effect: one AT command, no RRC of its own. */
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
 * beyond `len` bytes; treat it as a fixed-length buffer. */
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

#ifdef __cplusplus
}
#endif

#endif /* NET_H */

/* net_connect_guard.h — pure decision logic for two related MQTT-connect bug
 * fixes from the 22 Sep 2026 outage/evening session:
 *
 *  M1 (morning outage): "CONNECT issued, no CONNECTED/SUBSCRIBED seen" needs
 *  a bounded timer so a missed event cannot hang the device forever (root
 *  cause: the ESP32 light-slept 110ms after issuing the connect, RTS
 *  deasserted, and the CONNECTED event was never serviced -- see
 *  .overnight-handoff.md's "Outage 22 Sep 08:09-14:40 UTC" section).
 *
 *  M3 (22 Sep evening): once net_session_up() has failed
 *  NET_SESSION_UP_FAIL_ESCALATE times in a row because the modem refused to
 *  (re)configure or connect its MQTT client (mqttConfig()/mqttConnect()
 *  answering +CME ERROR because the modem's own client is still up --
 *  phaseO-recover.log), a full modem reset is warranted rather than backing
 *  off forever.
 *
 * Split the same way coverage.h is (see that header's own module comment):
 * no ESP-IDF dependency, driven by a caller-supplied clock (`int64_t now_us`,
 * esp_timer_get_time()-shaped but never called directly from here), so
 * firmware/host/test_net_connect_guard.c can run every transition without a
 * device. net.cpp is the only caller; it owns every actual modem call
 * (mqttConnect()/mqttDisconnect()) and every actual clock read -- this
 * module only decides WHEN/whether, never calls anything itself.
 */
#ifndef NET_CONNECT_GUARD_H
#define NET_CONNECT_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 30s: matches the existing s_resub_wait liveness-ping cap (net.cpp v0.2
 * §9.4's own comment: "two wake cycles plus RRC setup"). A CONNECT that has
 * produced neither CONNECTED nor SUBSCRIBED deserves no more patience than
 * the resubscribe that already uses this same bound, and esp_timer_get_time()
 * keeps counting across light sleep, so this bound is honoured even if the
 * device sleeps through most of it -- it only needs to be checked once on a
 * later wake, which net_service_session() already is (modes.c's own loop). */
#define NET_CONNECT_TIMEOUT_US ((int64_t) 30 * 1000000)

/* v0.2 M3: 3 consecutive net_session_up() failures at the mqttConfig()/
 * mqttConnect() step escalate to a full modem reset (rate_limited_modem_recover()
 * in modes.c) -- same threshold/shape as the existing 60s connect-watchdog's
 * own s_connect_watchdog_count in modes.c, which this is deliberately
 * consistent with rather than duplicating a different number for the same
 * kind of decision. */
#define NET_SESSION_UP_FAIL_ESCALATE 3u

typedef struct {
    bool wait;            /* a connect is outstanding, waiting on CONNECTED/SUBSCRIBED */
    int64_t issued_us;    /* when it was issued; meaningful only while wait */
    uint32_t fail_streak; /* consecutive net_session_up() failures, see NET_SESSION_UP_FAIL_ESCALATE */
} net_connect_guard_t;

void net_connect_guard_init(net_connect_guard_t *g);

/* Call the instant net_session_up() successfully queues mqttConnect(). Arms
 * the timeout and clears the fail streak -- a queued connect means the modem
 * accepted CONFIG/CONNECT this time, whatever happens next. */
void net_connect_guard_issued(net_connect_guard_t *g, int64_t now_us);

/* Call on SUBSCRIBED (the session is usable), on a FAILED CONNECTED (a
 * real answer, just a bad one), and from net_session_down() -- any of the
 * three proves the connect round trip is no longer silently unanswered. A
 * successful CONNECTED deliberately does NOT clear it: until SUBSCRIBED
 * arrives modes.c still sees mqtt_connected == false, and an unguarded
 * window there re-issued CONNECT on every boot (net.cpp's CONNECTED case)
 * (net_session_down() covers the case where the caller is deliberately
 * tearing the client down, e.g. the F3 permanent-failure path or a
 * host-detected-dead branch, and no new connect has been issued yet). Safe
 * to call when `wait` is already false. */
void net_connect_guard_clear(net_connect_guard_t *g);

/* True (and clears `wait`, one-shot like net.cpp's own s_resub_wait timeout)
 * the first call where a connect has been outstanding for at least
 * NET_CONNECT_TIMEOUT_US. False (no side effect) otherwise, including every
 * call after the first true one until the next net_connect_guard_issued(). */
bool net_connect_guard_check_timeout(net_connect_guard_t *g, int64_t now_us);

/* True while a connect is outstanding. This is what net_connect_in_flight()
 * (net.h) reports, and what modes.c's `!st.mqtt_connected` retry branch must
 * also consult before re-issuing mqttConnect() -- net_session_up() returning
 * true only means the AT command was queued, not that backoff should stop
 * being consulted (tonight's phaseO-recover2.log: a second AT+SQNSMQTTCONNECT
 * 110ms after the first, refused with +CME ERROR: 4). */
bool net_connect_guard_in_flight(const net_connect_guard_t *g);

/* Call whenever net_session_up() itself fails (configure_session() or
 * mqttConnect() refused) -- increments the streak. */
void net_connect_guard_note_fail(net_connect_guard_t *g);

/* True once note_fail() has been called NET_SESSION_UP_FAIL_ESCALATE times in
 * a row with no intervening net_connect_guard_issued()/reset call. */
bool net_connect_guard_should_escalate(const net_connect_guard_t *g);

/* Resets the fail streak only (e.g. after a full modem reset already run by
 * rate_limited_modem_recover()) without touching `wait`/`issued_us`. */
void net_connect_guard_reset_fail_streak(net_connect_guard_t *g);

#ifdef __cplusplus
}
#endif

#endif /* NET_CONNECT_GUARD_H */

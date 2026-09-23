/* publish_quiet.h — pure decision logic for the 23 Sep display-corruption
 * field failures: three separate register-loss events all correlated with a
 * panel SPI write (RAM write, or the register re-arm ahead of it) STARTING
 * while a pager-originated MQTT publish was in flight over the shared LTE
 * modem (a reconnect + `/up` ack publish in the 00:40 event; `/status
 * online` ~30ms ahead of a message render in an earlier one) — zero
 * corruption events correlated with a console-driven refresh (`disptest`),
 * which never publishes. The fix is a gate: a panel write must not start
 * between AT+SQNSMQTTPUBLISH being issued and its completion (the
 * WALTER_MODEM_MQTT_EVENT_PUBLISHED callback, driven by the modem's
 * +SQNSMQTTONPUBLISH URC or the AT command's own OK/ERROR), nor within
 * PUBLISH_QUIET_WINDOW_US after that.
 *
 * Split the same way net_connect_guard.h is (see that header's own module
 * comment): no ESP-IDF dependency, driven by a caller-supplied clock
 * (`int64_t now_us`, esp_timer_get_time()-shaped but never called directly
 * from here), so firmware/host/test_publish_quiet.c can run every
 * transition without a device. net.cpp is the only caller; it owns every
 * actual mqttPublish() call and every actual clock read — this module only
 * tracks how many publishes are outstanding and whether the quiet window
 * that follows the last one has elapsed.
 *
 * in_flight is a COUNT, not a flag: net_publish()/net_publish_raw() are
 * called from several modules (book.c, loc.c, modes.c, msg.c, setup.c,
 * sms.c) and nothing serialises them against each other, so two publishes
 * can legitimately be outstanding at once (e.g. a `/status` and a message
 * ack issued moments apart) — a bare bool would let the first PUBLISHED
 * event clear the gate while the second publish is still in flight.
 */
#ifndef PUBLISH_QUIET_H
#define PUBLISH_QUIET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 300ms: the coordinator's own figure for how long after a publish
 * completes its LTE uplink activity (and the electrical noise blamed for
 * the register loss) can still be settling. Not independently
 * bench-measured; PENDING_HW like every other timing constant in this
 * driver stack. */
#define PUBLISH_QUIET_WINDOW_US ((int64_t) 300 * 1000)

typedef struct {
    uint32_t in_flight;     /* publishes issued but not yet PUBLISHED-acked */
    int64_t quiet_until_us; /* valid only once in_flight has reached 0 at least
                              * once; 0 = "never armed", also treated as "not
                              * quiet" only while in_flight > 0 (see .c) */
} publish_quiet_gate_t;

void publish_quiet_gate_init(publish_quiet_gate_t *g);

/* Call the instant a publish is actually issued to the modem (mqttPublish()
 * returned true, i.e. AT+SQNSMQTTPUBLISH was queued) — increments in_flight. */
void publish_quiet_gate_issued(publish_quiet_gate_t *g);

/* Call on the PUBLISHED event (the +SQNSMQTTONPUBLISH URC, or the modem's
 * OK/ERROR terminating that command), whether it reports success or
 * failure — either way the round trip is no longer outstanding. Decrements
 * in_flight (clamped at 0: a spurious/duplicate event must not underflow),
 * and once it reaches 0 arms the quiet window from `now_us`. */
void publish_quiet_gate_done(publish_quiet_gate_t *g, int64_t now_us);

/* True while a panel write should hold off: a publish is outstanding, or one
 * completed less than PUBLISH_QUIET_WINDOW_US ago. False once both
 * conditions are clear. */
bool publish_quiet_gate_should_wait(const publish_quiet_gate_t *g, int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* PUBLISH_QUIET_H */

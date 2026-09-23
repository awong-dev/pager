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

/* 23 Sep release-build fix (docs/PROTOCOL.md field failure, "44-byte publish
 * corruption"): net_sleep() must not deassert RTS while a publish's AT round
 * trip (command line -> '>' data prompt -> payload bytes -> OK/ERROR) is
 * still in flight, or the modem is left waiting for payload bytes that never
 * arrive — the next retry's own command line then gets consumed as that
 * leftover payload (observed on the bench: a 44-byte retried command line
 * was accepted as the 44-byte payload the first attempt had promised, and
 * published verbatim, tripping the relay's bad-sig check). 15s is bounded
 * well BELOW the vendored library's own worst-case command timeout
 * (CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS, 30s/attempt, up to
 * WALTER_MODEM_DEFAULT_CMD_ATTEMPTS retries) on purpose: this hold only needs
 * to outlast a NORMAL publish round trip (the data-prompt exchange plus
 * whatever RRC reconnect time it costs if the modem had gone idle -- a few
 * seconds), not a fully wedged AT transaction. The other side of that
 * trade-off is deliberate too: a lost PUBLISHED URC (done() never called)
 * must never pin the device awake indefinitely, so this hold gives up after
 * 15s and lets the device sleep even though in_flight is still nonzero --
 * see publish_quiet_gate_hold_sleep()'s own doc comment for how a later
 * issued() recovers from that case instead of inheriting the stale
 * timestamp. */
#define PUBLISH_SLEEP_HOLD_MAX_US ((int64_t) 15 * 1000 * 1000)

typedef struct {
    uint32_t in_flight;     /* publishes issued but not yet PUBLISHED-acked */
    int64_t quiet_until_us; /* valid only once in_flight has reached 0 at least
                              * once; 0 = "never armed", also treated as "not
                              * quiet" only while in_flight > 0 (see .c) */
    int64_t issued_us;      /* issue time of the OLDEST outstanding publish,
                              * valid only while in_flight > 0 -- see
                              * publish_quiet_gate_issued()/_done()'s own
                              * comments for how this is maintained without
                              * keeping a full per-publish timestamp list. */
} publish_quiet_gate_t;

void publish_quiet_gate_init(publish_quiet_gate_t *g);

/* Call the instant a publish is actually issued to the modem (mqttPublish()
 * returned true, i.e. AT+SQNSMQTTPUBLISH was queued) — increments in_flight.
 *
 * issued_us bookkeeping (there is no per-publish timestamp list, only one
 * "oldest outstanding" estimate): set to `now_us` whenever in_flight was 0
 * (this publish is the new oldest), and ALSO whenever the existing hold has
 * already expired (`now_us - issued_us >= PUBLISH_SLEEP_HOLD_MAX_US`) even
 * though in_flight was already > 0 -- that second case is what stops a
 * stale in-flight count left behind by a lost URC (done() that never came)
 * from suppressing this brand-new publish's own hold window; without it a
 * publish issued minutes after a forgotten one would inherit that forgotten
 * one's already-expired timestamp and get zero hold protection of its own.
 * Otherwise (a second publish issued while the first is still genuinely
 * within its own hold window) the timestamp is left alone -- the oldest
 * outstanding publish is still the right one to gate on. */
void publish_quiet_gate_issued(publish_quiet_gate_t *g, int64_t now_us);

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

/* True while net_sleep() should be skipped: a publish is outstanding AND the
 * oldest outstanding one was issued less than PUBLISH_SLEEP_HOLD_MAX_US ago
 * -- see that macro's own comment for why 15s and why this is deliberately
 * NOT the same condition as publish_quiet_gate_should_wait() (that one gates
 * a panel write and is allowed to wait indefinitely while in_flight > 0;
 * this one gates sleep and must give up eventually so a lost URC cannot pin
 * the device awake). False once in_flight is 0, or once the oldest
 * outstanding publish's own 15s has elapsed. */
bool publish_quiet_gate_hold_sleep(const publish_quiet_gate_t *g, int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* PUBLISH_QUIET_H */

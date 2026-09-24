/* resub_verdict.h — pure decision function for S2's SUBACK-collision fix
 * (docs/SLEEP_URC_DESIGN.md §6, "SUBACK collision").
 *
 * A plain function of scalar inputs, not a stateful module like
 * net_connect_guard.h: xport_lte.cpp's own s_resub_wait/s_resub_sent_us/
 * s_resub_second_try/s_last_down_ingest_us stay exactly where they are
 * (individual `volatile`, single-writer-per-field, shared between the MQTT
 * event task and lte_service_session()'s caller task -- the same discipline
 * the rest of that file's state already documents). Wrapping them in one
 * non-volatile struct here would be a new, untested threading hazard for no
 * benefit. This function only decides what a 30s no-SUBACK timeout means; it
 * is called with a snapshot of those fields and never itself touches
 * WalterModem, esp_timer, or any state -- which is what lets
 * firmware/host/test_resub_verdict.c exercise every case without a device.
 */
#ifndef RESUB_VERDICT_H
#define RESUB_VERDICT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 30s: matches the existing bound (docs/SLEEP_URC_DESIGN.md §6, "far earlier
 * than the modem's own ~6 minute silent resume"). */
#define RESUB_VERDICT_TIMEOUT_US ((int64_t) 30 * 1000000)

typedef enum {
    RESUB_VERDICT_NONE = 0, /* no re-SUBSCRIBE outstanding, or it has not timed out yet */
    RESUB_VERDICT_ALIVE,    /* a /down message ingested after `sent_us` proves the session alive */
    RESUB_VERDICT_RETRY,    /* first re-SUBSCRIBE unanswered, no downlink proof: send a second one */
    RESUB_VERDICT_DEAD,     /* the second re-SUBSCRIBE (second_try) also went unanswered */
} resub_verdict_t;

/* wait: a re-SUBSCRIBE is currently outstanding (s_resub_wait).
 * sent_us: esp_timer_get_time() when it was sent (s_resub_sent_us).
 * last_down_ingest_us: esp_timer_get_time() at the last successfully
 *   ingested /down message, 0 if none yet this boot (s_last_down_ingest_us).
 * second_try: the outstanding re-SUBSCRIBE is already a retry of an earlier
 *   one this liveness cycle (s_resub_second_try).
 * now_us: esp_timer_get_time() now.
 * Pure query: never mutates anything (there is nothing to mutate -- all
 * inputs are by value). The caller applies the verdict's state changes
 * itself (docs/SLEEP_URC_DESIGN.md §6 lists them per verdict). */
resub_verdict_t resub_verdict_check(bool wait, int64_t sent_us, int64_t last_down_ingest_us,
                                     bool second_try, int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* RESUB_VERDICT_H */

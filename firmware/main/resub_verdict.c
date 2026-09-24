/* resub_verdict.c — see resub_verdict.h for the module comment. */
#include "resub_verdict.h"

resub_verdict_t resub_verdict_check(bool wait, int64_t sent_us, int64_t last_down_ingest_us,
                                     bool second_try, int64_t now_us)
{
    if (!wait) {
        return RESUB_VERDICT_NONE;
    }
    if ((now_us - sent_us) <= RESUB_VERDICT_TIMEOUT_US) {
        return RESUB_VERDICT_NONE;
    }
    if (last_down_ingest_us > sent_us) {
        return RESUB_VERDICT_ALIVE;
    }
    if (!second_try) {
        return RESUB_VERDICT_RETRY;
    }
    return RESUB_VERDICT_DEAD;
}

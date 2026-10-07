/* refreshpol.h -- when to spend a full e-ink refresh (owner decision, 7 Oct 2026).
 *
 * Every keystroke (Enter included) is a PARTIAL update; partials fade existing
 * black, so a dirty counter counts partials since the last full and the
 * policy below decides when a full is worth ~3 s of drawing:
 *
 *   1. dirty < FLOOR: never an extra full.
 *   2. dirty >= FLOOR: full at the first gap of IDLE_S with no key input.
 *   3. dirty >= FLOOR: a whole-screen transition (send, leave chat, screen
 *      switch) is drawn as a full instead of a partial.
 *   4. pre-sleep (the "zz" render before rail_off()): full if dirty >=
 *      PRESLEEP_MIN (and > 0), so an idle wake that drew a clock tick costs nothing.
 *   5. dirty >= CEILING: full at the next gap of GAP_MS between keys.
 *   Never a full mid-burst (time since last key < GAP_MS), with two
 *   exceptions: pre-sleep, and a transition (the transition IS the key that
 *   just arrived, e.g. Enter to send, so a burst guard would veto it always).
 *
 * The core (struct + refreshpol_*) is pure C, no ESP-IDF, host-tested by
 * firmware/host/test_refreshpol.c. The device half (global instance, NVS
 * knobs, disp.c hooks) is under #ifdef ESP_PLATFORM in refreshpol.c.
 *
 * Power effect: none by itself (counters); a granted full costs ~3 s of panel
 * drive. Threading: fields are word-sized; written by the modes task and the
 * debug console only. */
#ifndef REFRESHPOL_H
#define REFRESHPOL_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PAGER_REFRESH_FLOOR
#define PAGER_REFRESH_FLOOR 8
#endif
#ifndef PAGER_REFRESH_CEILING
#define PAGER_REFRESH_CEILING 24
#endif
#ifndef PAGER_REFRESH_IDLE_S
#define PAGER_REFRESH_IDLE_S 4
#endif
#ifndef PAGER_REFRESH_PRESLEEP_MIN
#define PAGER_REFRESH_PRESLEEP_MIN 3
#endif
#ifndef PAGER_REFRESH_GAP_MS
#define PAGER_REFRESH_GAP_MS 1500
#endif

typedef enum {
    REFRESHPOL_REASON_OTHER = 0, /* a full nobody asked this module for (boot, recovery, ...) */
    REFRESHPOL_REASON_IDLE,
    REFRESHPOL_REASON_TRANSITION,
    REFRESHPOL_REASON_PRESLEEP,
    REFRESHPOL_REASON_CEILING,
} refreshpol_reason_t;

typedef struct {
    uint32_t dirty;          /* partials since the last full */
    int64_t last_key_us;     /* valid only if key_seen */
    int64_t last_partial_us; /* 0 until the first partial */
    bool key_seen;
    uint32_t floor;
    uint32_t ceiling;
    uint32_t idle_s;
    uint32_t gap_ms;
    uint32_t presleep_min; /* rule 5 fires only when dirty >= this (and > 0) */
    refreshpol_reason_t reason; /* why the last refreshpol_want_full() returned true */
} refreshpol_t;

void refreshpol_init(refreshpol_t *p);                         /* compile-time defaults, dirty 0 */
void refreshpol_on_key(refreshpol_t *p, int64_t now_us);       /* any key, Enter included */
void refreshpol_on_partial(refreshpol_t *p, int64_t now_us);   /* a partial reached the panel */
void refreshpol_on_full(refreshpol_t *p);                      /* a full completed: dirty = 0 */
/* True if a full refresh should be done now; sets p->reason when true
 * (OTHER when false). */
bool refreshpol_want_full(refreshpol_t *p, int64_t now_us, bool transition, bool pre_sleep);
const char *refreshpol_reason_str(refreshpol_reason_t r);

#ifdef ESP_PLATFORM
/* Device half. */
refreshpol_t *refreshpol_global(void);
void refreshpol_load_nvs(void); /* NVS "disp" floor/ceil/idle/gap overrides; call once after nvs_flash_init */
void refreshpol_save_nvs(void); /* persist the current knobs (console `refresh`) */
void refreshpol_note_key(void);        /* refreshpol_on_key(global, now) */
void refreshpol_note_partial(void);    /* disp.c hook: partial issued */
void refreshpol_note_full(void);       /* disp.c hook: full completed; logs one INFO line */
bool refreshpol_poll(bool transition, bool pre_sleep); /* want_full on the global */
#endif

#endif /* REFRESHPOL_H */

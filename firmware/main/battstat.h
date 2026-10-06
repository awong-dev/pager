/* battstat.h — battery statistics: time-in-state counters that ride /status
 * key 68 `bs` (docs/BATTERY_STATS_DESIGN.md B1-B5, docs/PROTOCOL.md §10).
 *
 * The board has no fuel gauge, so the pager counts how long it spends in each
 * state and the relay models the drain. One window of counters lives in an
 * RTC_NOINIT struct (own magic + CRC32, NOT inside pager_rtc_t: writing a
 * field there without rtc_save() would fail g_rtc's CRC and cold-boot the
 * message state). A successful /status publish SUBTRACTS what it carried, so
 * ms remainders carry over and nothing counted between build and publish is
 * lost; a failed publish subtracts nothing and the next /status carries a
 * larger window with the same `sq`.
 *
 * Layout: a pure core (bs_core_*, state passed in, no ESP-IDF; built on the
 * host by firmware/host/test_battstat.c) and device wrappers (battstat_*)
 * that gather the live inputs.
 *
 * THREADING: every battstat_* function EXCEPT battstat_raise() runs on the
 * modes_run() task only. battstat_raise() writes one RAM byte and is safe
 * from any task (a lost race costs one mislabelled iteration).
 *
 * Power effect of everything here: none (counters and one CRC32 over 88 B).
 */
#ifndef BATTSTAT_H
#define BATTSTAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BATTSTAT_MAGIC 0x42535431u /* "BST1" */
#define BATTSTAT_KEY 68            /* PROTOCOL.md §10 envelope key `bs` */

/* Awake-time cause = priority (higher wins) = wire order of `aw`. */
typedef enum { BS_TIMER = 0, BS_ATTN, BS_HOT, BS_UI, BS_MODEM, BS_FETCH, BS_NCAUSE } bs_cause_t;

typedef struct {
    uint32_t magic, crc; /* crc covers every byte after itself */
    uint32_t sq;
    uint32_t dt_ms, sl_ms, aw_ms[BS_NCAUSE], rl_ms, md_ms[3]; /* md: 0 off, 1 search, 2 gnss */
    uint32_t ns, x1, rf[3], cn, re;                           /* rf: full, partial, upgraded */
    uint16_t mvn, pad;
} battstat_rtc_t;
_Static_assert(sizeof(battstat_rtc_t) == 92, "battstat_rtc_t must be 92 B (BATTERY_STATS_DESIGN.md B2)");

/* Per-boot RAM state: since-boot source baselines all start at 0 because the
 * sources count since boot. */
typedef struct {
    int64_t last_tick_us;
    uint64_t slept_us;
    volatile uint8_t cause_max;
    uint32_t rail_ms, ext1, rf[3], link, radio;
} bs_ram_t;

typedef struct {
    int64_t now_us;
    uint32_t rail_ms, ext1, rf[3], link, radio; /* since-boot source counters */
    bool off, search, gnss;
} bs_inputs_t;

/* A window in wire units (seconds and counts). */
typedef struct {
    uint32_t sq, dt, sl, aw[BS_NCAUSE], ns, x1, rl, rf[3], mvn, cn, md[3], re;
} bs_snap_t;

/* Pure core. */
uint32_t bs_core_crc(const battstat_rtc_t *r);
bool bs_core_init(battstat_rtc_t *r);                 /* true if the window survived */
void bs_core_tick(battstat_rtc_t *r, bs_ram_t *ram, const bs_inputs_t *in);
void bs_core_sleep(battstat_rtc_t *r, bs_ram_t *ram, int64_t slept_us);
void bs_core_raise(bs_ram_t *ram, bs_cause_t c);
void bs_core_note_mv(battstat_rtc_t *r, int mv);
void bs_core_snapshot(const battstat_rtc_t *r, bs_snap_t *out);
bool bs_core_encode(cbor_w_t *w, const bs_snap_t *s);
void bs_core_commit(battstat_rtc_t *r, const bs_snap_t *s);

/* Device wrappers (modes_run task only, except battstat_raise). */
void battstat_init(void);                                  /* boot: validate or zero the window */
void battstat_tick(uint32_t link_total, bool radio_off);   /* loop top: charge the previous iteration */
void battstat_note_sleep(int64_t slept_us);                /* after net_sleep(): time spent inside it */
void battstat_raise(bs_cause_t c);                         /* any task: this iteration was at least `c` */
void battstat_note_mv(int mv);                             /* a valid battery reading */
void battstat_snapshot(bs_snap_t *out);
bool battstat_encode(cbor_w_t *w, const bs_snap_t *s);     /* writes key 68; call only when s->dt >= 1 */
void battstat_commit(const bs_snap_t *s);                  /* after a successful /status publish */
void battstat_format(char *buf, size_t cap);               /* one console line */

#ifdef __cplusplus
}
#endif

#endif /* BATTSTAT_H */

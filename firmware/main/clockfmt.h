/* clockfmt.h — pure status-bar clock text helpers (TASK_clock.md, owner
 * request 24 Sep ~11pm PDT: "do live clock when in use and replace with
 * --:-- when sleeping so there's no stale time"). No ESP-IDF/modem/RTC
 * dependency of its own — same "whole file, no ESP-IDF dependency" pattern
 * coverage.c/wifi_policy.c/publish_quiet.c use, split out from ui.c (which
 * is not itself host-buildable: driver/i2c.h, esp_log.h, FreeRTOS) purely so
 * this piece is (firmware/host/Makefile's test_clock rule). ui.c
 * (draw_status_bar()/ui_clock_due()) is the one production caller: it
 * gathers in_use (modes_in_use())/seeded+hh/mm (net_get_clock()) and hands
 * them here.
 */
#ifndef CLOCKFMT_H
#define CLOCKFMT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "--:--" whenever `in_use` or `seeded` is false (not in the attentive
 * window, or the clock has never been seeded from the network,
 * PROTOCOL.md §3.5 — never guess a wall time); else "HH:MM" from `hh`/`mm`
 * (caller's job to have already converted an epoch to local hour/minute —
 * UTC, this codebase has no timezone concept anywhere, same as
 * ui_format_hhmm()). `out` must be >= 6 bytes. */
void clockfmt_status_text(bool in_use, bool seeded, int hh, int mm, char *out, size_t out_size);

/* Minute-change (or in-use/seeded-state-change) detector: TASK_clock.md Do
 * #3's "compare the formatted HH:MM with the last drawn one on every loop
 * pass". `cur` is this call's freshly-formatted text (clockfmt_status_text()'s
 * output); `last` is the caller's own persistent "last drawn" buffer (at
 * least `last_size` bytes, NUL-terminated, `last_size` >= 6 matching
 * clockfmt_status_text()'s own contract). Returns true iff they differ, and
 * on that same call copies `cur` into `last` (so the next call with the
 * same `cur` returns false) — one dedup step, not two separate compare/
 * update calls, so a caller can never observe "due" without also having
 * just recorded it as drawn. Pure: a string compare + copy, no timer/modem
 * access of its own. */
bool clockfmt_due(const char *cur, char *last, size_t last_size);

#ifdef __cplusplus
}
#endif

#endif

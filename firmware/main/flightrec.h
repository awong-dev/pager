/* flightrec.h -- debug-build "flight recorder" for the sleep page-loss
 * investigation (docs/SLEEP_PAGE_LOSS_BRIEF.md §6 item A).
 *
 * Instrumentation only: records every raw UART byte in/out (via the
 * vendored library's WalterModem::setPagerTraceHook(), PATCHES.md 1.17)
 * plus the wake/RTS/CTS/probe/sleep events net.cpp and modes.c feed it,
 * into a PSRAM ring that survives light sleep (the USB console does not).
 * Dumped on the console (`flightrec` command, main.c) once the console is
 * back, or automatically after a `sleeptest` window closes.
 *
 * Debug build only (PAGER_DEBUG_NO_LIGHT_SLEEP): every symbol below still
 * exists in a release build, as a trivial inline no-op, so call sites in
 * net.cpp/modes.c/main.c never need their own #ifdef -- the library hook
 * itself (WalterModem.h/.cpp) is the one piece that is NOT #ifdef-gated
 * (PATCHES.md 1.17's own note), because it is a null-checked function
 * pointer with zero cost when unset, and flightrec_init() only ever sets
 * it in a debug build.
 *
 * Recording is off by default even in a debug build (so ordinary boot
 * traffic is never captured): flightrec_set_recording(true) arms it,
 * flightrec_set_recording(false) disarms it. flightrec_clear() resets the
 * ring and t0 but does not by itself change the recording flag -- modes.c
 * calls both together at a sleeptest window's start.
 */
#ifndef FLIGHTREC_H
#define FLIGHTREC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP

/* Allocates the ring (96 KiB MALLOC_CAP_SPIRAM; falls back to 16 KiB
 * MALLOC_CAP_8BIT if PSRAM allocation fails, logging which) and installs
 * the library's trace hook (WalterModem::setPagerTraceHook(), PATCHES.md
 * 1.17). Call once from app_main(), before any modem bring-up -- the hook
 * must be installed before WalterModem::begin() so a fresh recording never
 * misses the first bytes of a boot it is later armed to capture. The hook
 * itself is inert (every flightrec_* call below is a no-op) until
 * flightrec_set_recording(true). No modem/sleep-state effect: one PSRAM
 * allocation and one function-pointer store. */
void flightrec_init(void);

/* Resets the write index and dropped-record count and stamps a fresh t0
 * (esp_timer_get_time()) that every recorded event's timestamp is printed
 * relative to. Does not itself start or stop recording -- pair with
 * flightrec_set_recording() at a window's start, same as
 * modes_debug_sleeptest_start() does. */
void flightrec_clear(void);

/* Arms (true) or disarms (false) recording. While disarmed, the hook and
 * every flightrec_event()/flightrec_bytes() call below return immediately
 * -- ordinary boot/idle traffic outside a sleeptest window is never
 * captured. */
void flightrec_set_recording(bool on);

/* The current light-sleep count within the open window (modes.c's
 * s_st_sleeps), stamped into every record from here on so the dump can be
 * read wake-by-wake. Call right after net_sleep() returns. */
void flightrec_set_cycle(uint16_t n);

/* Records one event: kind (see flightrec.c's module comment for the full
 * kind/label table) plus up to two signed 32-bit fields, meaning dependent
 * on kind. No-op while not recording. */
void flightrec_event(char kind, int32_t a, int32_t b);

/* Records one byte record: kind ('R' raw UART in, 'T' UART out, 'U'
 * unpaired response buffer) plus up to 240 bytes of payload -- longer
 * payloads are truncated to 240 bytes and flagged (the dump then prints
 * "...(+N)" for the remainder). No-op while not recording. */
void flightrec_bytes(char kind, const uint8_t *data, size_t len);

/* gpio_get_level(CONFIG_WALTER_MODEM_PIN_CTS) -- a plain read, available
 * (and meaningful) regardless of the recording flag, so callers can sample
 * it to detect a transition even when they only conditionally choose to
 * record one. Returns -1 if the ring was never allocated (flightrec_init()
 * not yet called). */
int flightrec_cts_level(void);

/* The number of records currently held (0 after flightrec_clear(), or if
 * flightrec_init() was never called). Used by the console `flightrec`
 * summary line and by modes.c's post-window hold decision. */
uint32_t flightrec_count(void);

/* Prints every record, oldest first, one line per record -- see
 * flightrec.c's module comment for the exact format. Safe to call whether
 * or not a window is currently open; printf() only (works from the console
 * task or modes_run()'s task). */
void flightrec_dump(void);

#else /* !PAGER_DEBUG_NO_LIGHT_SLEEP: trivial no-op stubs, no flightrec.c in this build */

static inline void flightrec_init(void) { }
static inline void flightrec_clear(void) { }
static inline void flightrec_set_recording(bool on) { (void) on; }
static inline void flightrec_set_cycle(uint16_t n) { (void) n; }
static inline void flightrec_event(char kind, int32_t a, int32_t b)
{
    (void) kind;
    (void) a;
    (void) b;
}
static inline void flightrec_bytes(char kind, const uint8_t *data, size_t len)
{
    (void) kind;
    (void) data;
    (void) len;
}
static inline int flightrec_cts_level(void) { return -1; }
static inline uint32_t flightrec_count(void) { return 0; }
static inline void flightrec_dump(void) { }

#endif /* PAGER_DEBUG_NO_LIGHT_SLEEP */

#ifdef __cplusplus
}
#endif

#endif /* FLIGHTREC_H */

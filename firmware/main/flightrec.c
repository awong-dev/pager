// flightrec.c -- debug-build "flight recorder" (see flightrec.h's module
// comment). Instrumentation only: no effect on the sleep cycle, the probe,
// the wait or the library's own parsing -- this file only ever reads
// already-occurring events and appends them to a PSRAM ring.
//
// Whole file compiles to nothing outside PAGER_DEBUG_NO_LIGHT_SLEEP
// (flightrec.h supplies trivial inline stubs for that case instead).

#include "flightrec.h"

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "net.h" // net_debug_install_trace_hook() (PATCHES.md 1.17)

static const char *TAG = "flightrec";

// docs/SLEEP_PAGE_LOSS_BRIEF.md §6 item A's own budget: a whole sleeptest
// window's worth of UART traffic at a few hundred bytes/wake, comfortably
// inside 96 KiB. 16 KiB is a last-resort fallback (~60-70 short records)
// if PSRAM is unavailable, so the recorder still captures *something*
// rather than refusing to run.
#define FLIGHTREC_CAP_PRIMARY (96u * 1024u)
#define FLIGHTREC_CAP_FALLBACK (16u * 1024u)
#define FLIGHTREC_MAX_BYTES 240u
#define FLIGHTREC_FLAG_TRUNCATED 0x01u

// Packed, byte ring, no wrap (per the task brief): `bytes[len]` (`len` is
// this record's *stored* payload length, always <= FLIGHTREC_MAX_BYTES --
// see flightrec_bytes()'s own comment on `a` doubling as the overflow
// count for a truncated byte record) trails immediately after the header
// in the arena, no padding.
typedef struct __attribute__((packed)) {
    int64_t t_us;   // relative to s_t0
    uint16_t cycle;
    uint8_t kind;
    uint8_t flags; // bit0 = truncated
    uint16_t len;
    int32_t a;
    int32_t b;
} flightrec_hdr_t;

static uint8_t *s_buf = NULL;
static size_t s_cap = 0;
static size_t s_wr = 0; // next free byte offset in s_buf
static uint32_t s_dropped = 0;
static uint32_t s_count = 0;
static int64_t s_t0 = 0;
static uint16_t s_cycle = 0;
static volatile bool s_recording = false;
static bool s_inited = false;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// The library's own trace hook (PATCHES.md 1.17): matches
// `walter_pager_trace_fn` structurally (net.h's wrapper takes a plain
// function pointer of the same signature so this file never needs to
// include the C++-only WalterModem.h). Just forwards to flightrec_bytes(),
// which is itself a no-op while not recording.
static void flightrec_trace_hook(char kind, const uint8_t *data, size_t len)
{
    flightrec_bytes(kind, data, len);
}

void flightrec_init(void)
{
    s_buf = heap_caps_malloc(FLIGHTREC_CAP_PRIMARY, MALLOC_CAP_SPIRAM);
    if (s_buf != NULL) {
        s_cap = FLIGHTREC_CAP_PRIMARY;
        ESP_LOGI(TAG, "flightrec_init: %u KiB PSRAM ring allocated",
                 (unsigned) (s_cap / 1024u));
    } else {
        s_buf = heap_caps_malloc(FLIGHTREC_CAP_FALLBACK, MALLOC_CAP_8BIT);
        if (s_buf != NULL) {
            s_cap = FLIGHTREC_CAP_FALLBACK;
            ESP_LOGI(TAG,
                     "flightrec_init: PSRAM allocation failed; %u KiB internal-RAM ring "
                     "allocated instead",
                     (unsigned) (s_cap / 1024u));
        } else {
            s_cap = 0;
            ESP_LOGI(TAG, "flightrec_init: allocation failed (PSRAM and internal RAM both "
                          "refused) -- flight recorder disabled, CTS reads still work");
        }
    }
    s_wr = 0;
    s_dropped = 0;
    s_count = 0;
    s_cycle = 0;
    s_t0 = esp_timer_get_time();
    s_recording = false;
    s_inited = true;
    // Installed unconditionally (even if the allocation above failed): a
    // hook with nowhere to write just calls flightrec_bytes(), which is a
    // silent no-op once s_cap == 0 (see write_record() below). No power
    // effect: one function-pointer store in the vendored library.
    net_debug_install_trace_hook(flightrec_trace_hook);
}

void flightrec_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_wr = 0;
    s_dropped = 0;
    s_count = 0;
    s_t0 = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

void flightrec_set_recording(bool on)
{
    s_recording = on;
}

void flightrec_set_cycle(uint16_t n)
{
    s_cycle = n;
}

// Common writer for both flightrec_event() (len == 0, data == NULL) and
// flightrec_bytes(). A ring with no wrap: once a record would not fit in
// what remains of the arena, every further record (of either kind) is
// just counted in `dropped` and dropped, never partially written.
static void write_record(char kind, int32_t a, int32_t b, const uint8_t *data, uint16_t len,
                          uint8_t flags)
{
    if (!s_recording || s_cap == 0) {
        return;
    }
    size_t need = sizeof(flightrec_hdr_t) + (size_t) len;
    portENTER_CRITICAL(&s_lock);
    if (s_wr + need > s_cap) {
        s_dropped++;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    flightrec_hdr_t hdr;
    hdr.t_us = esp_timer_get_time() - s_t0;
    hdr.cycle = s_cycle;
    hdr.kind = (uint8_t) kind;
    hdr.flags = flags;
    hdr.len = len;
    hdr.a = a;
    hdr.b = b;
    memcpy(s_buf + s_wr, &hdr, sizeof(hdr));
    s_wr += sizeof(hdr);
    if (len > 0 && data != NULL) {
        memcpy(s_buf + s_wr, data, len);
        s_wr += len;
    }
    s_count++;
    portEXIT_CRITICAL(&s_lock);
}

void flightrec_event(char kind, int32_t a, int32_t b)
{
    write_record(kind, a, b, NULL, 0, 0);
}

void flightrec_bytes(char kind, const uint8_t *data, size_t len)
{
    uint8_t flags = 0;
    size_t stored = len;
    if (stored > FLIGHTREC_MAX_BYTES) {
        stored = FLIGHTREC_MAX_BYTES;
        flags |= FLIGHTREC_FLAG_TRUNCATED;
    }
    // A byte record ('R'/'T'/'U') has no other use for `a`/`b` (those are
    // event-kind fields, see flightrec_dump()'s switch below), so the
    // truncated overflow count rides in `a` -- this is what lets the dump
    // print "...(+N)" for a long line without a third length field on
    // every record just for this case. Documented here and in PATCHES.md/
    // the task report, not in the brief itself.
    int32_t overflow = (int32_t) (len - stored);
    write_record(kind, overflow, 0, data, (uint16_t) stored, flags);
}

int flightrec_cts_level(void)
{
    if (!s_inited) {
        return -1;
    }
    return gpio_get_level((gpio_num_t) CONFIG_WALTER_MODEM_PIN_CTS);
}

uint32_t flightrec_count(void)
{
    return s_count;
}

static void print_escaped(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\r') {
            fputs("\\r", stdout);
        } else if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c >= 0x20 && c < 0x7f) {
            putchar((int) c);
        } else {
            printf("\\x%02x", (unsigned) c);
        }
    }
}

void flightrec_dump(void)
{
    printf("flightrec: %u records, %u bytes, dropped=%u, t0=%lld\n", (unsigned) s_count,
           (unsigned) s_wr, (unsigned) s_dropped, (long long) s_t0);
    size_t off = 0;
    while (off + sizeof(flightrec_hdr_t) <= s_wr) {
        flightrec_hdr_t hdr;
        memcpy(&hdr, s_buf + off, sizeof(hdr));
        off += sizeof(hdr);
        if (off + hdr.len > s_wr) {
            break; // defensive: a corrupt header must not read past s_wr
        }
        const uint8_t *payload = s_buf + off;
        off += hdr.len;

        double t_s = (double) hdr.t_us / 1000000.0;
        printf("+%8.3f c%02u %c  ", t_s, (unsigned) hdr.cycle, (char) hdr.kind);
        switch ((char) hdr.kind) {
        case 'R':
        case 'T':
        case 'U':
            putchar('"');
            print_escaped(payload, hdr.len);
            putchar('"');
            if (hdr.flags & FLIGHTREC_FLAG_TRUNCATED) {
                printf("...(+%d)", (int) hdr.a);
            }
            break;
        case 'W':
            printf("cause=%d buffered=%d", (int) hdr.a, (int) hdr.b);
            break;
        case 'F':
            printf("cts=%d", (int) hdr.a);
            break;
        case 'C':
            printf("cts=%d dt_ms=%d", (int) hdr.a, (int) hdr.b);
            break;
        case 'P':
            printf("cts=%d", (int) hdr.a);
            break;
        case 'A':
            printf("answer_ms=%d", (int) hdr.a);
            break;
        case 'G':
            printf("waited_ms=%d", (int) hdr.a);
            break;
        case 'S':
            printf("planned_ms=%d cts=%d", (int) hdr.a, (int) hdr.b);
            break;
        case 'M':
            printf("latency_s=%d", (int) hdr.a);
            break;
        case 'X':
            printf("ms=%d cts=%d", (int) hdr.a, (int) hdr.b);
            break;
        case 'K':
        case 'E':
            break; // no a/b, per the task brief's event-kind table
        default:
            printf("a=%d b=%d", (int) hdr.a, (int) hdr.b);
            break;
        }
        putchar('\n');
    }
}

#endif /* PAGER_DEBUG_NO_LIGHT_SLEEP */

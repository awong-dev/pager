/* ui.h — CardKB keyboard input and the thread-view/composer screens.
 *
 * docs/DEVICE_TASKS.md F6.1 split this file: the SSD1680 transport moved to
 * disp.c/disp.h (BUSY handling, partial/full refresh, the 20-partial
 * cadence) and the framebuffer/text/glyph primitives moved to gfx.c/gfx.h
 * (Noto-derived fonts from the mmap'd `assets` partition, replacing the
 * retired hand-drawn 5x7 font). This header's public API is unchanged by
 * that split — modes.c, the only caller, needed no changes.
 *
 * Authority: docs/PROTOCOL.md §9.5 (frame buffer sizing — now disp.h/gfx.h's
 * concern), §9.4 (composer limit; docs/DEVICE_PLAN.md §5.2 withdraws the
 * old "reply body is ASCII" assumption in favor of UTF-8, 160 code
 * points/320 bytes). firmware/README.md (20-partial-refresh cadence, now
 * disp.c's).
 *
 * No third-party display component: firmware-architect rejected
 * cleishm/idfxx_epaper_ssd1680 (idf: '>=5.5' in its manifest vs. this
 * project's release-v5.2 toolchain). disp.c is a from-scratch driver
 * against the SSD1680 command set.
 *
 * Every timing/current/visual claim in this module (and in disp.c/gfx.c)
 * is PENDING_HW — no device is attached to the session that wrote it, and
 * on-glass legibility of the Noto-derived fonts is UNVERIFIED: there is no
 * way to render or photograph the framebuffer in this environment (the
 * host-side PNG render in firmware/host/render_png.c is the closest
 * approximation available pre-hardware, per docs/DEVICE_PLAN.md §5.2).
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ui_poll_keys() returns a small struct rather than a bare enum: composer
 * text entry needs the actual character, which a single "UI_KEY_NONE"-style
 * enum value cannot carry. UI_KEY_NONE below is a compound-literal constant
 * of this type so call sites can still write `if (k.type == UI_KEY_NONE)`-
 * shaped checks via the type field. */
typedef enum {
    UI_KEYTYPE_NONE = 0,
    UI_KEYTYPE_CHAR,      /* ch = printable ASCII 0x20-0x7E */
    UI_KEYTYPE_BACKSPACE, /* CardKB 0x08 */
    UI_KEYTYPE_ENTER,     /* CardKB 0x0D — "send" keypress, see ui.c's composer notes */
} ui_key_type_t;

typedef struct {
    ui_key_type_t type;
    char ch;
} ui_key_t;

#define UI_KEY_NONE ((ui_key_t) { .type = UI_KEYTYPE_NONE, .ch = 0 })

/* Bring up the panel: VCC gate on, hardware reset, SSD1680 init sequence.
 * Returns false if BUSY never deasserts (15s timeout, one retry) — the
 * caller MUST keep running headless in that case (PROTOCOL.md: no pager
 * function may be gated on the display). Forces a full refresh on the
 * first ui_render_thread() call after a successful init. */
bool ui_init(void);

/* Sleep the panel (0x10) and power the VCC gate off. Safe to call even if
 * ui_init() failed or the display was marked dead. */
void ui_shutdown(void);

/* Full thread view: top band = newest message, middle = up to 2 more,
 * bottom = status line. Follows the 20-partial-refresh cadence (every 20th
 * call that would otherwise be a partial does a full refresh instead and
 * resets the counter); a no-op (logs once) if the display was marked dead
 * this boot. */
void ui_render_thread(void);

/* Redraws just the top band (newest message) and forces a partial refresh,
 * regardless of the 20-partial counter. Used after msg_mark_shown()/
 * msg_mark_read() so the "NEW" marker disappears without a full repaint. */
void ui_render_message_pane(void);

/* Transient overlay message (e.g. "nothing to send", "keyboard not found").
 * Implemented as a partial refresh of the status band; the next
 * ui_render_thread()/ui_render_message_pane() call overwrites it. */
void ui_show_toast(const char *text);

/* Opens the reply composer (clears msg.c's composer buffer) and draws the
 * initial composer screen (follows the normal partial/full cadence — only
 * ui_init() and ui_composer_close() are specified to force a full refresh).
 * reply_to_id is accepted but unused (no threaded replies). */
void ui_composer_open(const char *reply_to_id);

/* Closes the composer. `sent` is cosmetic (caller has already called
 * msg_queue_reply() if appropriate) — this just clears the buffer and
 * forces a full refresh back to the thread view. */
void ui_composer_close(bool sent);

bool ui_composer_is_open(void);

/* Non-blocking CardKB poll. Calls modes_note_activity() whenever it returns
 * a real key (Part A bug #2). Never blocks the wake-and-drain loop on I2C:
 * on 3 consecutive NACKs the composer is closed with a toast and the
 * keyboard is treated as absent until the next ui_composer_open(). */
ui_key_t ui_poll_keys(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */

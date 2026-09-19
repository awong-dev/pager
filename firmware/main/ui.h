/* ui.h — the screen stack, status bar, and the CardKB read (docs/DEVICE_TASKS.md
 * F6.3, docs/DEVICE_PLAN.md §5.4/§5.5).
 *
 * Rewrite over the F6.1-era ui.c (which only drew the single legacy thread
 * view + composer). This header now owns:
 *   - the screen stack (`ui_screen_t {render, on_key, on_event}`, depth <= 4,
 *     push/pop/replace, §5.4),
 *   - the status bar (drawn by ui_render() itself, not by any screen —
 *     §5.4's bucket list: signal bars, link ok/x, unsent n, a lock icon iff
 *     `ident`'s IDENT_FLAG_REQ_SIG is set — NOT the device-passcode lock,
 *     which is lock.c/F6.5, not built yet — unread n, battery segments),
 *   - the CardKB I2C read (moved here from the old ui.c per F6.2's own scope
 *     note in input.h: input.c decodes bytes and owns the event queue, but
 *     something has to keep doing the I2C transaction, and F6.2 left that
 *     to whichever module F6.3 turned into the real screen-owning one),
 *   - the incoming-message "steal the screen" policy (§5.5).
 *
 * Screens actually built this task (§5.5): Home, Chat, Device, Setup —
 * `scr_home.c`, `scr_chat.c`, `scr_device.c`, `scr_setup.c`, matching this
 * task's Files list exactly (docs/DEVICE_TASKS.md F6.3). Two screens named
 * in DEVICE_PLAN.md §6's module map are NOT built here and are not declared
 * below: `scr_pick.c` (new-message recipient picker) and `scr_book.c`
 * (address book) both need `book.c` (F7.1, does not exist yet); `scr_lock.c`
 * (the Locked screen) needs `lock.c` (F6.5, does not exist yet). Every place
 * below a book.c/lock.c-dependent feature would otherwise appear (Home's
 * "New message"/"Address book"/"Lock now" rows, Device's passcode/auto-lock/
 * "Set up again" rows) is stubbed — see scr_home.c's and scr_device.c's own
 * module comments for exactly what and why.
 *
 * Compatibility note: `setup.c` (F3.5, not touched by this task — not in
 * F6.3's Files list) calls exactly two entry points from this header:
 * `ui_init()` and `ui_show_toast()`, both before `modes_boot()` ever runs
 * (no screen stack exists yet at that point — see setup.h's own module
 * comment on why the two power cycles never overlap). Both signatures are
 * unchanged from the pre-F6.3 header specifically so setup.c did not need
 * touching.
 *
 * All timing/current/visual claims here (and in disp.c/gfx.c) are
 * PENDING_HW.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>

#include "gfx.h"   /* gfx_font_t, used by ui_text_size() */
#include "input.h" /* input_key_t */

#ifdef __cplusplus
extern "C" {
#endif

#define UI_STACK_DEPTH 4

/* Screen lifecycle notifications (docs/DEVICE_PLAN.md §5.4's `on_event`).
 * Only ENTER exists today — fired on push/replace and on ui_pop() revealing
 * a screen underneath. Deliberately does NOT include an "incoming message"
 * event: the steal-or-toast policy (§5.5) is centralized in ui_incoming()
 * below rather than dispatched to whichever screen happens to be on top, so
 * screens don't each need to reimplement it. */
typedef enum {
    UI_EVT_ENTER,
} ui_evt_t;

typedef struct ui_screen_s {
    const char *name; /* logging only */

    /* Draws this screen's body AND its own footer line (docs/DEVICE_PLAN.md
     * §5.4: "the screen's body, and a footer of key hints" — footer text
     * differs per screen per the §5.5 mockups, so each screen draws its own
     * rather than ui_render() imposing one). Called with the framebuffer
     * already gfx_clear()'d and the status bar already drawn at
     * y < UI_BODY_TOP; must not draw above that line. Never called directly
     * by a screen — only from ui_render(). */
    void (*render)(void);

    /* Dispatches one decoded key/nav event to whichever screen is on top.
     * May call ui_push()/ui_pop()/ui_replace()/ui_go_home(). Never renders
     * itself — ui_render() (called once per modes_run() iteration by
     * modes.c) is the only render path, closing README R4/R5 together. */
    void (*on_key)(input_key_t key);

    /* Lifecycle notification; may be NULL. */
    void (*on_event)(ui_evt_t evt);
} ui_screen_t;

/* Screen singletons — one static instance each, defined in their own
 * scr_*.c, declared here (rather than a separate registry header) so
 * ui.c/scr_home.c can reference push targets without adding a file outside
 * this task's Files list. */
extern const ui_screen_t g_scr_home;
extern const ui_screen_t g_scr_chat;
extern const ui_screen_t g_scr_device;
extern const ui_screen_t g_scr_setup;
/* F6.5 (docs/DEVICE_PLAN.md §5.8): the Locked screen — lock.c now exists, so
 * this is no longer one of the book.c/lock.c-dependent screens this header's
 * own module comment above says are not built yet (that note is now stale
 * for this one screen only; scr_pick.c/scr_book.c still need book.c, F7.1). */
extern const ui_screen_t g_scr_lock;
/* F7.2 (docs/DEVICE_PLAN.md §5.5 "New message → pick recipient", "Address
 * book", "Nicknames"): book.c (F7.1) now exists too, so these are the last
 * two book.c-dependent screens this header's module comment above still
 * names as not-yet-built — that note is now fully stale. Not yet reachable
 * from Home (scr_home.c's HROW_NEWMSG/HROW_BOOK rows still show their
 * pre-F7.1 stub) — see scr_pick.c's/scr_book.c's own module comments for
 * why wiring that is flagged rather than done here. */
extern const ui_screen_t g_scr_pick;
extern const ui_screen_t g_scr_book;

/* scr_greeting.c: boot splash + reused-layout "sleeping" screen — see that
 * file's own module comment for the two modes.c call sites that own its
 * push/pop lifecycle. Not part of DEVICE_PLAN.md §5.5's screen set. */
typedef enum {
    GREETING_HELLO = 0,
    GREETING_SLEEPING,
} scr_greeting_mode_t;
void scr_greeting_set_mode(scr_greeting_mode_t mode);
extern const ui_screen_t g_scr_greeting;

/* scr_chat.c's own "mark every currently-visible down message read"
 * (docs/DEVICE_PLAN.md §5.5's Chat bullet) — exported so Home's "open chat"
 * path and ui_on_button_short() can call it right after a *user-initiated*
 * open. Deliberately NOT called from scr_chat's own on_event(UI_EVT_ENTER):
 * the incoming-message steal path (ui_incoming(), below) also pushes Chat,
 * and §5.5 is explicit that *that* path acks `shown` only, never `read` —
 * see ui_incoming()'s own comment. */
void scr_chat_mark_visible_read(void);

/* Bring up the panel (disp_init()) and the CardKB I2C bus, establish the
 * screen stack as [Home]. Power effect: see disp_init()'s own comment —
 * this adds no power effect of its own beyond that and the (idle, no
 * transaction yet) I2C peripheral init. Returns false on the same terms
 * disp_init() does; the caller MUST keep running headless in that case
 * (every ui_* entry point below already degrades to a no-op/log when the
 * display was marked dead, so screens/scr_*.c never need their own
 * disp_is_dead() checks). */
bool ui_init(void);

/* Sleep the panel and power its VCC gate off. Power effect: see
 * disp_shutdown()'s own comment. Not called anywhere in this task (no
 * caller ever wants the UI permanently off) — kept for symmetry/future use
 * (e.g. a future "ship mode"), same as it was pre-F6.3. */
void ui_shutdown(void);

/* ---------------------------------------------------------------------
 * Screen stack.
 * --------------------------------------------------------------------- */

/* Pushes `scr` on top (no-op, logged, if the stack is already at
 * UI_STACK_DEPTH). Fires scr->on_event(UI_EVT_ENTER). Does not render —
 * call ui_render() afterward (modes.c's per-iteration call already does,
 * for every path except ui_incoming()'s own synchronous render, see
 * below). */
void ui_push(const ui_screen_t *scr);

/* Pops the top screen, revealing the one underneath (no-op if depth <= 1 —
 * Home is always the floor of the stack, matching §5.5's "esc nothing" on
 * Home and "long = Home from anywhere" never leaving an empty stack).
 * Fires the revealed screen's on_event(UI_EVT_ENTER). */
void ui_pop(void);

/* Swaps the top of the stack for `scr` in place (depth unchanged). Fires
 * scr->on_event(UI_EVT_ENTER). If the stack is empty, behaves like
 * ui_push(). */
void ui_replace(const ui_screen_t *scr);

/* Resets the stack to exactly [Home] and fires Home's on_event(UI_EVT_ENTER)
 * — docs/DEVICE_PLAN.md §5.5's "long = Home from anywhere". */
void ui_go_home(void);

/* NULL if the stack is empty (should not happen after ui_init(); guarded
 * against anyway in ui_render()). */
const ui_screen_t *ui_top(void);

/* ---------------------------------------------------------------------
 * Rendering — call ONLY from modes_run()'s own task (firmware/README.md
 * R5's fix, docs/DEVICE_TASKS.md F6.3's Do: "render on the main task").
 * --------------------------------------------------------------------- */

/* Status bar height in px (icons are a fixed 12x12, gfx.h — §5.4's nominal
 * "10 px" is rounded up to fit them without clipping); screens must not
 * draw above this + the 1px separator hline ui_render() draws at
 * y == UI_STATUS_H. */
#define UI_STATUS_H 12
#define UI_BODY_TOP (UI_STATUS_H + 1)

/* Clears the framebuffer, draws the status bar, calls the top screen's
 * render(), then disp_partial_refresh() — ALWAYS a partial, never the
 * 20-partial cadence's full refresh (docs/DEVICE_PLAN.md §5.4: "the full
 * refresh is never taken on the inbound-message path... it is deferred to
 * the moment the UI-awake window lapses"). This is the render path called
 * on every wake-and-drain iteration while input_awake() is true — often
 * several times per interactive session — so a due full refresh must NOT
 * fire from here; see ui_on_awake_lapse() below for where it does. A no-op
 * status-bar-only content change costs exactly one partial-refresh row
 * window — disp.c's own diff against its shadow plane is what makes that
 * true, not any bucket-caching logic here (see disp_partial_refresh()'s
 * "no-op if nothing changed" contract). Power effect: ~0.3-0.8s, PENDING_HW,
 * unless disp_is_dead(), in which case this is a cheap no-op (framebuffer
 * math only, no SPI). */
void ui_render(void);

/* Same content-painting as ui_render(), but via disp_refresh_cadence() —
 * the one call site (besides ui_on_awake_lapse() below) allowed to consume
 * disp_init()'s "force a full refresh on the first call" priming
 * (disp.h), so the panel gets a clean baseline image on power-up instead
 * of a partial refresh diffed against a blank shadow plane. Called exactly
 * once, from modes_boot(), right after a successful ui_init(). Power
 * effect: ~2-4s (forced full), PENDING_HW. */
void ui_render_boot(void);

/* Called from modes.c on the input_awake() true->false edge (the UI-awake
 * window lapsing). Does not repaint — the framebuffer already holds
 * whatever the most recent ui_render() call painted — it only lets the
 * 20-partial cadence counter decide partial vs. full and sends whichever
 * is due, so a due full refresh lands here (interaction just ended) rather
 * than mid-interaction or on the inbound-message path. Power effect:
 * ~0.3-0.8s (partial, the common case — usually a no-op besides, since
 * content did not change) or ~2-4s (full, every 20th), PENDING_HW. */
void ui_on_awake_lapse(void);

/* Routes one decoded key/nav event to the top screen's on_key(). Does not
 * render — the caller (modes.c) calls ui_render() once after draining every
 * event for a given wake-and-drain iteration. */
void ui_dispatch_key(input_key_t key);

/* Global button semantics, docs/DEVICE_PLAN.md §5.5 (Home's Keys bullet,
 * stated as applying "from anywhere", not just on Home): short press opens
 * the newest unread chat and marks every message it renders `read` (via
 * scr_chat_mark_visible_read()), or does nothing if there is no unread
 * message; long press always goes Home. Neither renders — same contract as
 * ui_dispatch_key(). */
void ui_on_button_short(void);
void ui_on_button_long(void);

/* Incoming-message steal-the-screen policy (docs/DEVICE_PLAN.md §5.5):
 * `from` is the sender alias (already copied by the caller — modes.c's
 * render_pending handoff). `was_asleep` is whether PAGER_MODE was SLEEP
 * immediately before this message (modes.c captures that before its own
 * set_mode(ACTIVE, ...) call, since this function no longer has any other
 * way to tell "asleep" apart from "awake and on Home").
 *
 * If the device was asleep, or Home is on top: pushes Chat (WITHOUT calling
 * scr_chat_mark_visible_read() — §5.5 is explicit that this path acks
 * `shown` only, not `read`), renders synchronously (disp_partial_refresh(),
 * bypassing the refresh cadence per §5.4's "never taken on the inbound-
 * message path"), and returns true. The caller MUST call msg_mark_shown()
 * immediately afterward, and ONLY in that case (§4: "after the e-paper
 * refresh completes, never before"; README R7: never claim `shown` for a
 * message that was not actually displayed).
 *
 * Otherwise (composing, or any screen other than Home is on top): shows a
 * one-line toast ("new: <from>") without touching the screen stack, and
 * returns false — the message is NOT marked shown; it stays MSG_ACK_UNSHOWN
 * until the student actually opens the chat later, at which point
 * scr_chat_mark_visible_read()'s msg_mark_read() call promotes it directly
 * (PROTOCOL.md §4.1 rule 2's "read without a prior shown" back-fill is what
 * makes that correct, not a bug). */
bool ui_incoming(const char *from, bool was_asleep);

/* Transient overlay message (e.g. "not available yet", "reply too long").
 * Implemented as a partial refresh of the bottom text row; the next
 * ui_render() call overwrites it. Kept for setup.c's pre-modes_boot() calls
 * (see this header's own compatibility note) and used internally by
 * ui.c/scr_*.c. Power effect: ~0.3-0.8s (disp_partial_refresh()),
 * PENDING_HW; a no-op/log-only if disp_is_dead(). */
void ui_show_toast(const char *text);

/* Non-blocking CardKB I2C poll + decode, called once per modes_run()
 * iteration whenever input_awake() is true (mirrors the pre-F6.3 ui.c's
 * composer-only poll, now generalised to every screen — see input.h's
 * scope note on why input.c does not do this itself). Feeds each decoded
 * byte to input_feed_key() (input.h), which arms the UI-awake window and
 * queues an INPUT_EVT_KEY event for modes.c's existing event-drain loop to
 * hand to ui_dispatch_key(). On 3 consecutive I2C failures, logs once and
 * backs off until the next call (same "keyboard not found" tolerance the
 * pre-F6.3 composer had) — never blocks the wake-and-drain loop. Power
 * effect: one I2C read per call (~0.1 ms, negligible next to the 100ms
 * poll cadence input_awake() already implies). */
void ui_poll_keyboard(void);

/* ---------------------------------------------------------------------
 * Text size setting (docs/DEVICE_PLAN.md §5.2: "a Setting (normal/large)
 * stored in NVS; the body uses it, the status bar and footer are always
 * GFX_FONT_NORMAL"). NVS namespace "ui", key "textsz". Toggled from the
 * Device screen (§5.5).
 * --------------------------------------------------------------------- */
gfx_font_t ui_text_size(void);
void ui_toggle_text_size(void); /* power effect: one NVS (flash) write */

/* ---------------------------------------------------------------------
 * Small formatting helper shared by scr_home.c/scr_chat.c (message
 * timestamps, §5.5's mockups: "14:02" style, never a date — the ring only
 * ever holds a few hours of history). UTC (this codebase has no timezone
 * concept anywhere — PROTOCOL.md's `ts` is always epoch seconds). Writes
 * "--:--" for epoch_s == 0 (no network clock ever obtained this boot,
 * PROTOCOL.md §3.5 — never guess a wall time). `out` must be >= 6 bytes.
 * --------------------------------------------------------------------- */
void ui_format_hhmm(int64_t epoch_s, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */

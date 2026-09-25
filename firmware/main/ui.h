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
#include <stdint.h> /* uint32_t, ui_kb_skipped_read_count() */

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
/* TASK_ui_finish.md Do #4/#6 (owner list, 24 Sep 22:30 PDT): the Locked
 * screen's own idle->entering transition (its "locked" -> "password:" +
 * masked field). Called ONLY from ui_on_button_short() (ui.c) below, on an
 * IO1 short press while g_scr_lock is on top and idle — Do #6: "Unlocking
 * is only started by the IO1 button ... not by typing." No-op if already
 * entering (scr_lock.c's own doc comment). No modem or sleep-state effect:
 * a screen-local UI state flip only. */
void scr_lock_start_entry(void);
/* F7.2 (docs/DEVICE_PLAN.md §5.5 "New message → pick recipient", "Address
 * book", "Nicknames"): book.c (F7.1) now exists too. T3
 * (docs/CHAT_UI_DESIGN.md §3/§5) wires both of these in from Home's
 * "New message"/"Address book" rows — see scr_home.c/scr_pick.c's own
 * module comments. */
extern const ui_screen_t g_scr_pick;
extern const ui_screen_t g_scr_book;

/* scr_greeting.c: boot splash screen — see that file's own module comment
 * for the call sites that own its push/pop lifecycle. Not part of
 * DEVICE_PLAN.md §5.5's screen set. */
typedef enum {
    GREETING_HELLO = 0,
} scr_greeting_mode_t;
void scr_greeting_set_mode(scr_greeting_mode_t mode);

/* Optional footer line under the "Hi ...!" banner. Added for main.c's
 * pre-provisioning boot screen (no ident/no SIM yet, so it reuses this
 * screen rather than standing up a whole scr_boot.c): "booting" ->
 * "sim missing"/found -> "shutting down".
 * NULL/"" (the default) draws no footer at all, so modes_boot()'s normal
 * post-ident-load push is unaffected. Copies into a fixed internal buffer;
 * `status` need not outlive the call. */
void scr_greeting_set_status(const char *status);
extern const ui_screen_t g_scr_greeting;

/* scr_chat.c's own "mark every currently-visible down message read"
 * (docs/DEVICE_PLAN.md §5.5's Chat bullet) — exported so Home's "open chat"
 * path and ui_on_button_short() can call it right after a *user-initiated*
 * open. Deliberately NOT called from scr_chat's own on_event(UI_EVT_ENTER):
 * the incoming-message steal path (ui_incoming(), below) also pushes Chat,
 * and §5.5 is explicit that *that* path acks `shown` only, never `read` —
 * see ui_incoming()'s own comment. */
void scr_chat_mark_visible_read(void);

/* T4 (docs/CHAT_UI_DESIGN.md §3 "Chat"/"Pick"): the user-initiated "open
 * this peer's chat" entry point — pushes Chat (firing its own
 * on_event(UI_EVT_ENTER), which resets the composer/scroll), sets the
 * current peer (scr_chat_set_peer(), below) to `alias` (NULL/"" selects the
 * book's default peer), then marks every now-visible row read
 * (scr_chat_mark_visible_read()). Replaces T3's scr_chat_open_with_prefix()
 * (removed): the screen itself now filters to one peer, so there is no more
 * composer `@alias` prefill to seed — a plain Enter already addresses the
 * right peer (scr_chat.c's try_send()). */
void scr_chat_open_peer(const char *alias);

/* T4: sets the current peer WITHOUT pushing/replacing the screen stack or
 * marking anything read — for ui_incoming()'s own steal-the-screen path
 * (below), which may need to switch an ALREADY-open Chat to a newly
 * arrived page's peer (docs/CHAT_UI_DESIGN.md §3: "incoming page ... opens
 * that page's peer") without re-firing on_event(UI_EVT_ENTER) (that would
 * wipe an in-progress reply to whatever peer was open before, every single
 * time, even when the incoming page is for the SAME peer already on
 * screen). Only resets the composer/scroll when `alias` actually differs
 * from the peer already showing — same NULL/""-means-default convention as
 * scr_chat_open_peer() above. */
void scr_chat_set_peer(const char *alias);

/* Bring up the panel (disp_init()) and the CardKB I2C bus, establish the
 * screen stack as [Home]. Power effect: see disp_init()'s own comment —
 * this adds no power effect of its own beyond that and the (idle, no
 * transaction yet) I2C peripheral init. Returns false on the same terms
 * disp_init() does; the caller MUST keep running headless in that case
 * (every ui_* entry point below already degrades to a no-op/log when the
 * display was marked dead, so screens/scr_*.c never need their own
 * disp_is_dead() checks). */
bool ui_init(void);

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
 * y == UI_STATUS_H.
 *
 * 14, not 12: confirmed on real hardware that 12 was too short and the
 * separator hline was drawn right through the bottom of the status bar's
 * own text (draw_status_bar()'s gfx_text() calls at y=1) -- measured
 * directly from the real font asset data (tools/mkassets.py's encoded
 * baseline=13 for the 12px block, i.e. already taller than the nominal
 * "12px" label), text drawn at line_top_y=1 bottoms out at row 13 for an
 * ordinary (non-descender) glyph, one row past the old UI_STATUS_H=12. */
#define UI_STATUS_H 14
#define UI_BODY_TOP (UI_STATUS_H + 1)

/* ---------------------------------------------------------------------
 * Shared list-row geometry (TASK_ui_round2.md Do #2). Moved here verbatim
 * from scr_home.c's now-removed HOME_ROW_H/HOME_SEP_H/home_row_advance()
 * (Do #1, 24 Sep photo-evidence fix: DejaVu Sans's own measured
 * baseline=13 + 3px descent for g/y/j/p/q at the 12px size — see that
 * commit's own derivation, unchanged here) so every fixed-pitch 12px-font
 * list screen (Home, Pick, Book, Device) shares one row box height and one
 * "double underline below the last row" drawing, instead of each screen
 * re-deriving (or under-deriving) its own and re-introducing the same
 * descender-overlap bug in a new place. scr_chat.c is NOT switched to this
 * fixed pitch — its own row pitch is font-size-dependent (GFX_FONT_NORMAL/
 * GFX_FONT_LARGE body text, `pitch` in scr_chat.c) and its existing
 * message-list/composer hline already has clearance built into that
 * variable pitch; see this task's own report for why forcing the fixed
 * UI_ROW_H there was left alone rather than guessed at.
 *
 * ui_row_advance() is `static inline` here (not a real function body in
 * ui.c) specifically so it stays host-testable without linking any
 * ESP-IDF-dependent object file: firmware/host/test_home_list.c gets it by
 * just `#include "ui.h"`, the same "one shared source of truth for both the
 * real renderer and its own host test" discipline home_row_advance() itself
 * established before this move. gfx.h/input.h (this header's own two
 * includes) are both already host-safe (no ESP-IDF headers of their own),
 * so this adds no new host-build dependency.
 * --------------------------------------------------------------------- */
#define UI_ROW_H 16 /* 12px-font row box: baseline(13) + descent(3) */
#define UI_SEP_H 8  /* row-to-separator/next-row clearance: +3/+5 offsets below, then y += 8 */

static inline int ui_row_advance(int row_top, bool is_separator)
{
    return row_top + (is_separator ? UI_SEP_H : UI_ROW_H);
}

/* Draws the double-underline separator immediately below the last list row,
 * at `y` == that row's own top (already advanced past it by the caller's
 * own ui_row_advance() loop) — two hlines at y+3/y+5, 3px clear of the
 * row's own deepest descender, same offsets HOME_SEP_H's own doc comment
 * (pre-move) measured. A real function in ui.c (not inline): it draws
 * (gfx_hline()) rather than just computing a position, and no host test
 * needs the actual pixels, only ui_row_advance()'s geometry above. */
void ui_draw_row_separator(int y);

/* Footer key-hints row shared by every scr_*.c (§5.4/§5.5: "a footer of key
 * hints"), always GFX_FONT_NORMAL. The old convention drawn everywhere was
 * `GFX_SCREEN_H - 9`, which confirmed-broke on real hardware
 * (scr_greeting.c's status footer, "shutting down", visibly cut off at the
 * bottom) -- the 12px font's real baseline is 13 (see UI_STATUS_H's own
 * comment above), so `-9` put more than half of every footer's own
 * descenders, and often the whole baseline-and-below body of the line,
 * past row 127 where gfx_set_pixel() silently clips it.
 *
 * -18, not the earlier fix's -16: re-measured against DejaVu Sans, now the
 * shipped default face (TASK_ui_finish.md Do #2, owner decision 24 Sep —
 * Noto is no longer the default asset), not Noto. Decoding
 * build/images/assets-dejavu.bin's own 12px block (tools/mkassets.py's PGFA
 * format, gfx.h) gives g/y/j/p/q all bottoming out `rows - bearing_y` == 3px
 * below the pinned baseline=13, i.e. row 16 relative to line_top_y — 2px
 * deeper than Noto's own row 14 the -16 value was measured against (Noto's
 * shallow 1px descent at this size). -18 keeps the same "one row of margin
 * before the panel's bottom edge" convention: the deepest real descender
 * (row 16 relative to line_top_y) sits at row 126 when line_top_y ==
 * GFX_SCREEN_H-18 == 110. If the shipped default face changes again, re-run
 * the same decode (see scr_home.c's HOME_ROW_H comment for the exact
 * python snippet used) rather than guessing. */
#define UI_FOOTER_Y (GFX_SCREEN_H - 18)

/* Lazy rail gate (TASK_ui_round2.md Do #4, docs/ROADMAP.md): the one place
 * every render path brings the 3V3 peripheral rail up on demand instead of
 * relying on the wake path to have already done it unconditionally. Calls
 * rail_on() (rail.h) and, iff the rail was OFF just before that call,
 * disp_note_power_loss() (disp.h) — the same "panel RAM was lost, force a
 * full refresh" note the old unconditional wake-path rail_on() used to give
 * on every rail-was-off wake, now given only at the point a render is
 * actually about to happen. Called from the top of ui_render() (below) and
 * ui_incoming()'s own synchronous render path — see rail.h's
 * own module comment for the other three rules (boot, an EXT0/EXT1 wake,
 * and the attentive window) that also bring the rail on, independently of
 * this function. A no-op call (no rail edge, no disp_note_power_loss()) if
 * the rail is already on, which is the common case inside the attentive
 * window or right after an EXT0/EXT1 wake. Power effect: powers the
 * display/CardKB/LIS3DH on iff they were off — see rail_on()'s own comment
 * for the rail edge itself. */
void ui_ensure_powered(void);

/* Count of off->on rail edges ui_ensure_powered() itself drove (a render
 * that needed the rail up on its own — NOT modes.c's separate wake-path
 * EXT0/EXT1 rule) since boot. Free-running, never reset — bench diagnostic
 * (modes.c's sleeptest report: "rail: on_wakes=... lazy_on="). */
uint32_t ui_rail_lazy_on_count(void);

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
 * event for a given wake-and-drain iteration. Also clears the boot crash
 * indicator (see ui_set_crash_indicator() below) on its way in, if it was
 * still showing -- "the first keyboard interaction", any key event, not
 * gated on lock state or which screen is on top. */
void ui_dispatch_key(input_key_t key);

/* Call once, at boot (main.c, right after watchdog_boot()), if
 * watchdog_last_reset_was_crash() is true: raises a small icon
 * (GFX_ICON_CRASH, gfx.h) in the status bar's right-hand cluster,
 * immediately left of the TLS padlock slot, drawn by every draw_status_bar()
 * call until ui_dispatch_key() sees the first key press -- a page arriving
 * (ui_incoming()) does not clear it. No modem or sleep-state effect: a
 * render-time flag only. */
void ui_set_crash_indicator(bool show);

/* Global button semantics, docs/DEVICE_PLAN.md §5.5 (Home's Keys bullet,
 * stated as applying "from anywhere", not just on Home): short press opens
 * the newest unread message's own peer chat (T4, docs/CHAT_UI_DESIGN.md §3
 * Do #5; scr_chat_set_peer()) and marks every message it renders `read`
 * (via scr_chat_mark_visible_read()), or does nothing if there is no unread
 * message; long press always goes Home. Neither renders — same contract as
 * ui_dispatch_key(). */
void ui_on_button_short(void);
void ui_on_button_long(void);

/* Incoming-message steal-the-screen policy (docs/DEVICE_PLAN.md §5.5;
 * docs/CHAT_UI_DESIGN.md §3 T4: "incoming page ... opens that page's
 * peer"). `from` is the sender alias — the message's own peer, msg.c's
 * msg_peer_of() rule (the group alias for a group message, never `sndr`),
 * already copied by the caller (modes.c's render_pending handoff).
 * `was_asleep` is whether PAGER_MODE was SLEEP immediately before this
 * message (modes.c captures that before its own set_mode(ACTIVE, ...) call,
 * since this function no longer has any other way to tell "asleep" apart
 * from "awake and on Home").
 *
 * If the device was asleep, Home is on top, or Chat is ALREADY on top (on
 * any peer): pushes/keeps Chat and switches it to `from`'s own filtered
 * view (scr_chat_set_peer(), ui.h) — WITHOUT calling
 * scr_chat_mark_visible_read() — §5.5 is explicit that this path acks
 * `shown` only, not `read`), renders synchronously (disp_partial_refresh(),
 * bypassing the refresh cadence per §5.4's "never taken on the inbound-
 * message path"), and returns true. The caller MUST call msg_mark_shown()
 * immediately afterward, and ONLY in that case (§4: "after the e-paper
 * refresh completes, never before"; README R7: never claim `shown` for a
 * message that was not actually displayed) — which is also why the
 * already-in-Chat case always switches peer rather than leaving a different
 * peer's chat on screen: see ui.c's own comment on this call site for the
 * trade-off (an in-progress reply to a DIFFERENT peer than the one that
 * just paged in is interrupted; the SAME peer's is not).
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
 * Implemented as a partial refresh of one text row at y=98 (docs/
 * CHAT_UI_DESIGN.md §4: clears rows 96..110, above UI_FOOTER_Y so it never
 * overlaps the footer hint); the next ui_render() call overwrites it. Kept
 * for setup.c's pre-modes_boot() calls (see this header's own compatibility
 * note) and used internally by ui.c/scr_*.c. Power effect: ~0.3-0.8s
 * (disp_partial_refresh()), PENDING_HW; a no-op/log-only if disp_is_dead(). */
void ui_show_toast(const char *text);

/* Non-blocking CardKB I2C poll + decode, called once per modes_run()
 * iteration whenever input_awake() is true (mirrors the pre-F6.3 ui.c's
 * composer-only poll, now generalised to every screen — see input.h's
 * scope note on why input.c does not do this itself). Feeds each decoded
 * byte to input_feed_key() (input.h), which arms the UI-awake window and
 * queues an INPUT_EVT_KEY event for modes.c's existing event-drain loop to
 * hand to ui_dispatch_key(). On 3 consecutive I2C failures, logs once and
 * backs off until the next call (same "keyboard not found" tolerance the
 * pre-F6.3 composer had) — never blocks the wake-and-drain loop. Rail gate
 * (docs/ROADMAP.md, owner 24 Sep 10:30 pm PDT): withholds the read (no I2C
 * transaction at all, counted in ui_kb_skipped_read_count()) for
 * PAGER_KB_BOOT_GUARD_MS (ui.c, 300 ms) after the most recent rail.c
 * rail_restored_us() edge — the CardKB MCU needs that long to boot after
 * its power (the 3V3 rail) returns; a read failing after that guard has
 * elapsed re-initializes the I2C driver once per restore edge. Power
 * effect: one I2C read per call (~0.1 ms, negligible next to the 100ms
 * poll cadence input_awake() already implies), or none while the guard is
 * withholding it. */
void ui_poll_keyboard(void);

/* Count of ui_poll_keyboard() calls withheld by the post-rail-restore
 * CardKB boot guard above, since boot. Free-running, never reset — bench
 * diagnostic (modes.c's sleeptest report: "kb_skipped_reads="). */
uint32_t ui_kb_skipped_read_count(void);

/* rail.c's own back-powering fix (owner, 24 Sep 11:15 pm PDT): the CardKB's
 * SDA/SCL pull-ups are tied to the always-on 3V3, not the gated peripheral
 * rail, so leaving the I2C driver's idle-high bus level up while rail_off()
 * drops the rail phantom-powers the CardKB MCU through its I/O protection
 * diodes (the "red LED stays on asleep" symptom). rail_off() calls this
 * BEFORE driving PAGER_PIN_3V3_EN high: deletes the I2C driver (if
 * installed) and reconfigures PAGER_PIN_KB_SDA/PAGER_PIN_KB_SCL (pins.h) as
 * plain GPIO outputs driven LOW — low, not floating, since floating still
 * lets the external pull-up feed the keyboard — and excludes both from
 * sleep GPIO isolation (gpio_sleep_sel_dis(), the same pattern net.cpp's
 * net_sleep() uses for the modem RTS line) so they hold LOW through every
 * light sleep instead of being re-pulled high. The LIS3DH (accel.c) shares
 * this same I2C bus and rail, so it is already unpowered whenever this
 * runs; driving its SDA/SCL low is still correct. Power effect: removes the
 * CardKB's phantom-power path through the I2C pull-ups while the rail is
 * off; no effect on the rail itself. */
void ui_kb_bus_release(void);

/* rail.c's rail_on() calls this AFTER driving PAGER_PIN_3V3_EN low: returns
 * PAGER_PIN_KB_SDA/PAGER_PIN_KB_SCL to the I2C driver (re-runs
 * i2c_kb_init()). The existing PAGER_KB_BOOT_GUARD_MS post-restore guard
 * (ui_poll_keyboard(), keyed off rail.c's rail_restored_us()) already
 * withholds the first read until the CardKB MCU has had time to boot, so no
 * additional guard is needed here. Power effect: none by itself — the rail
 * edge that powers the CardKB back up already happened in rail_on(); this
 * only restores the bus's I2C mode. */
void ui_kb_bus_restore(void);

/* Count of ui_kb_bus_release() calls since boot — one per rail_off() edge.
 * Free-running, never reset — bench diagnostic (modes.c's sleeptest
 * report: "kb_bus_releases="). */
uint32_t ui_kb_bus_release_count(void);

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
 * ever holds a few hours of history). Local time (TASK_ui_round2.md Do #7,
 * hardcoded "PST8PDT,M3.2.0,M11.1.0" per owner 25 Sep 2026 — a cfg field
 * later; main.c's app_main() sets TZ/tzset() once at boot, before any
 * caller here or in ui.c's compute_status_clock_text() runs) — was UTC
 * before this task (PROTOCOL.md's own `ts` wire field is still always epoch
 * seconds; only the on-glass HH:MM rendering changed). Writes
 * "--:--" for epoch_s == 0 (no network clock ever obtained this boot,
 * PROTOCOL.md §3.5 — never guess a wall time). `out` must be >= 6 bytes.
 * --------------------------------------------------------------------- */
void ui_format_hhmm(int64_t epoch_s, char *out, size_t out_size);

/* ---------------------------------------------------------------------
 * TASK_clock.md: the status bar's own live clock, at the right screen edge.
 * "--:--" whenever `in_use` is false (modes_in_use(): the pager is not in
 * the 120s attentive window) or `seeded` is false (net_get_clock() has never
 * returned a network time this boot, PROTOCOL.md §3.5), else "HH:MM" from
 * `hh`/`mm` (caller's job to have already converted an epoch to local
 * hour/minute — see draw_status_bar()'s own compute_status_clock_text(),
 * ui.c, for the only production caller). Pure formatter, no modem/RTC
 * access of its own — the identical logic is host-tested directly against
 * clockfmt.c/clockfmt.h (firmware/host/test_clock.c), which this function
 * just delegates to (ui.c).
 * `out` must be >= 6 bytes, same as ui_format_hhmm() above.
 * --------------------------------------------------------------------- */
void ui_status_clock_text(bool in_use, bool seeded, int hh, int mm, char *out, size_t out_size);

/* True iff the status bar's HH:MM has changed (a minute rolled over, or the
 * in-use/seeded state flipped) since draw_status_bar() last actually drew
 * it. Call once per modes_run() loop pass while modes_in_use() might be
 * true; a true return means the caller should render (ui_render()) to push
 * the new text out — see ui_clock_due()'s own doc comment (ui.c) for why
 * this is safe outside input.c's shorter 30s UI-awake window and cheap
 * (never an AT command) even every loop pass. Always false while not in
 * use — the one-shot "--:--" render on the attentive window lapsing is
 * modes_run()'s own job, not this function's. */
bool ui_clock_due(void);

/* TASK_net_interleave.md: True iff the status bar's link icon (net_registered()
 * ? MQTT link OK/X : --) or signal bars (net_registered() ? real reading : 0
 * bars) differ from what draw_status_bar() last actually drew — same "call
 * once per modes_run() loop pass, cheap RAM reads only, never an AT command"
 * contract as ui_clock_due() above, and NOT gated on modes_in_use() (unlike
 * ui_clock_due(): these icons matter while asleep/not-in-use too, e.g. the
 * boot registration edge landing with no keypress). Always false while
 * headless (disp_is_dead()) — nothing was ever drawn to compare against. */
bool ui_net_icons_due(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */

/* modes.h — mode state machine + RTC memory contract entry points.
 *
 * modes.c owns the RTC struct (docs/PROTOCOL.md §9), the single set_mode()
 * transition funnel (§11), the button short/long press state machine
 * (Phase 5 Part C), and the wake-and-drain loop (§8).
 */
#ifndef MODES_H
#define MODES_H

#include <stdbool.h>

/* One-time entry point. Call exactly once from app_main(), before
 * modes_run(). Validates/initialises the RTC struct, brings up the modem
 * and MQTT session (net_init()/net_session_up()), initialises msg.c/ui.c,
 * and leaves the device in sleep mode (HANDOFF.md §2: "Boot in sleep
 * mode"). */
void modes_boot(void);

/* The wake-and-drain loop (PROTOCOL.md §8). Never returns. */
void modes_run(void) __attribute__((noreturn));

/* Unconditionally bumps the active-mode idle deadline (Part A bug #2: the
 * deadline must be refreshed by activity while already active, not only set
 * once on the sleep->active edge). No-op while in sleep mode. Called from
 * the button state machine and from ui_poll_keys() on every real keypress. */
void modes_note_activity(void);

/* Read-only mode getter for ui.c's status line. Does not participate in the
 * set_mode() funnel — it only reads. */
bool modes_is_active(void);

#endif /* MODES_H */

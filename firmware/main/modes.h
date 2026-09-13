/* modes.h — mode state machine + RTC memory contract entry points.
 *
 * modes.c owns the RTC struct (docs/PROTOCOL.md §9), the single set_mode()
 * transition funnel (§11), and the wake-and-drain loop (§8). msg.c/ui.c
 * remain Phase 5 stubs; modes.c only logs incoming messages this phase.
 */
#ifndef MODES_H
#define MODES_H

/* One-time entry point. Call exactly once from app_main(), before
 * modes_run(). Validates/initialises the RTC struct, brings up the modem
 * and MQTT session (net_init()/net_session_up()), and leaves the device in
 * sleep mode (HANDOFF.md §2: "Boot in sleep mode"). */
void modes_boot(void);

/* The wake-and-drain loop (PROTOCOL.md §8). Never returns. */
void modes_run(void) __attribute__((noreturn));

#endif /* MODES_H */

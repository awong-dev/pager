/* net_internal.h — private wiring between net.cpp and xport_lte.cpp only.
 *
 * Never included by modes.c, net.h's other callers, or a future xport_wifi.c
 * (see net.h's own "what modes.c must not know" rule -- this header is the
 * mechanical seam behind it, not part of that contract). It exists purely
 * because docs/WIFI_TASKS.md W4 moved the MQTT session code out of net.cpp
 * into its own translation unit while its callers ("Stay in net.cpp,
 * LTE-only, untouched": net_init, net_bringup, net_recover_modem, the
 * PAGER_DEBUG_NO_LIGHT_SLEEP mqtttest/nettest diagnostics, net_bootstrap_*)
 * still touch a handful of the moved state directly, and vice versa.
 *
 * Every name below is byte-identical to its pre-move counterpart in net.cpp:
 * moving it here only changed `static` (translation-unit-local) to external
 * linkage (one file now defines it, the other `extern`-declares it) — no
 * variable was renamed, no function body changed. That is the whole of this
 * file's job: it is not new state, just the two moved-apart halves of what
 * was one file's worth of static storage.
 */
#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "net_connect_guard.h"
#include "publish_quiet.h"
#include "net.h"
#include "WalterModem.h"

/* ---------------------------------------------------------------------
 * Owned by net.cpp (LTE bringup/registration state, docs/WIFI_DESIGN.md
 * §1's "stay in net.cpp" list): net_bringup()/net_init()/net_recover_modem()
 * define and drive these. xport_lte.cpp's net_session_up() (now lte_session_up())
 * reads/writes s_registered and s_session_configured and calls
 * configure_session()/note_registration(), exactly as it did when they were
 * all one file.
 * --------------------------------------------------------------------- */
extern volatile bool s_registered;
extern bool s_session_configured;
extern char s_down_topic[48];
bool configure_session(void);
void note_registration(bool registered);

/* ---------------------------------------------------------------------
 * Owned by xport_lte.cpp (the moved MQTT session state,
 * docs/WIFI_DESIGN.md §1's "moved verbatim" list): net.cpp's net_init()
 * and net_recover_modem() reset these fresh on boot/F4 recovery, and the
 * PAGER_DEBUG_NO_LIGHT_SLEEP-only mqtttest/nettest diagnostics (net_check_tcp(),
 * net_check_mqtt()) read s_mqtt_connected/s_disconnect_edge directly, exactly
 * as they did when net_session_up()/pager_mqtt_event_handler() lived in the
 * same file.
 * --------------------------------------------------------------------- */
extern net_connect_guard_t s_connect_guard;
extern publish_quiet_gate_t s_publish_quiet;
extern volatile bool s_mqtt_connected;
extern volatile bool s_disconnect_edge;

/* RCA_SLEEP_PUBLISH.md §3 instrumentation: xport_lte.cpp's 12-entry publish
 * ring (see its own module comment above s_publish_ring). net.cpp's
 * net_get_publish_ring() (net.h) forwards to this directly -- always safe to
 * call regardless of the active transport, same as the state above. */
uint32_t lte_get_publish_ring(net_publish_ring_entry_t *out, uint32_t cap);

/* The MQTT event handler itself (moved to xport_lte.cpp), registered from
 * net.cpp's net_bringup() and net_bootstrap_attach() via
 * WalterModem::setMQTTEventHandler(). */
void pager_mqtt_event_handler(WMMQTTEventType event, const WMMQTTEventData *data, void *args);

/* ---------------------------------------------------------------------
 * Owned by net.cpp (net_set_msg_cb(): "registers one callback that both
 * transports call", docs/WIFI_DESIGN.md §1 -- a single registration that
 * outlives any one transport, not per-transport state). xport_lte.cpp's
 * MESSAGE case invokes it exactly as it did before the move.
 * --------------------------------------------------------------------- */
extern void (*s_msg_cb)(const char *topic, const char *body, uint16_t len);

#endif /* NET_INTERNAL_H */

/* net_xport.h — the transport seam (docs/WIFI_DESIGN.md §1, docs/WIFI_TASKS.md
 * W4). net.h stays the only API modes.c sees; every net.h entry point that
 * touches the MQTT session dispatches, inside net.cpp, through exactly one
 * `net_xport_ops_t` selected by the single `s_active_xport` variable net.cpp
 * owns. This header is that vtable's shape plus the transport enum — it is
 * consumed only by net.cpp (the dispatcher) and by xport_lte.cpp / a future
 * xport_wifi.c (the implementations); modes.c never includes it.
 *
 * `net_xport_t` lives in net.h, not here: net.h's own net_xport_active()
 * returns it, and net.h must stay includable from modes.c (a plain C file)
 * without pulling in this header. Keeping the enum in net.h and having this
 * header simply use it avoids a circular include between the two.
 *
 * This task (W4) adds exactly one implementation, `xport_lte_ops()`
 * (xport_lte.cpp) — the moved, unmodified LTE MQTT session code from net.cpp,
 * behind this vtable's field names instead of its own. WiFi (W5) adds a
 * second, `xport_wifi_ops()`, and `net_xport_switch()` becomes the sole
 * writer of `s_active_xport`. Neither exists yet.
 */
#ifndef NET_XPORT_H
#define NET_XPORT_H

#include <stdint.h>
#include <stdbool.h>

#include "net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* net_session_up()/net_session_down() */
    bool (*up)(void);
    void (*down)(void);

    /* net_publish()/net_publish_raw()/net_publish_quiet_wait_ms() */
    bool (*publish)(const char *topic, char *buf, uint16_t len, uint8_t qos);
    bool (*publish_raw)(const char *topic, uint8_t *buf, uint16_t len, uint8_t qos);
    uint32_t (*publish_quiet_wait_ms)(uint32_t max_wait_ms);

    /* net_service_session() -- net.cpp applies the LTE-only suppression
     * (net_set_lte_suppressed()) before calling this, so an implementation
     * never sees it and never needs to know it exists. */
    void (*service)(void);

    /* net_get_mqtt_status() / the two edge-ack calls */
    void (*status)(net_mqtt_status_t *out);
    void (*ack_disconnect_edge)(void);
    void (*ack_session_restart_edge)(void);

    /* net_modem_busy() / net_connect_in_flight() / net_connect_fail_streak_maxed() */
    bool (*modem_busy)(void);
    bool (*connect_in_flight)(void);
    bool (*connect_fail_streak_maxed)(void);

    /* net_take_memfull_delta() / net_take_oversize_delta() */
    uint32_t (*take_memfull_delta)(void);
    uint32_t (*take_oversize_delta)(void);
} net_xport_ops_t;

/* Implemented in xport_lte.cpp: returns the ops table wired to the moved,
 * unmodified LTE MQTT session code (net_session_up/down,
 * pager_mqtt_event_handler, net_service_session, etc. under this vtable's
 * field names -- see that file's own module comment). net_init() registers
 * this as the (today, only) active transport. */
const net_xport_ops_t *xport_lte_ops(void);

/* Implemented in xport_wifi.c (docs/WIFI_TASKS.md W5): esp-mqtt over the
 * ESP32-S3's own WiFi station (wifi_sta.c/.h), TLS via esp-tls/mbedtls,
 * reusing ident_get_ca()'s pinned PEM and net_connect_guard/publish_quiet
 * unchanged. Refuses to bring a session up (`.up()` returns false) unless
 * catrust_get_state() == CATRUST_PINNED (docs/WIFI_DESIGN.md §5: no
 * plaintext fallback on WiFi, ever). net_xport_switch() (net.cpp) is the
 * only caller that selects this transport. */
const net_xport_ops_t *xport_wifi_ops(void);

/* net.cpp's own trampoline to the single callback registered via
 * net_set_msg_cb() (net.h: "registers one callback that both transports
 * call"). xport_wifi.c calls this instead of reaching into
 * net_internal.h's s_msg_cb directly -- that header is deliberately
 * restricted to net.cpp/xport_lte.cpp (its own module comment: "never ...
 * a future xport_wifi.c"), the mechanical leftover of the W4 file split,
 * not part of this vtable's own contract. No-op if no callback is
 * registered yet. */
void net_dispatch_msg(const char *topic, const char *body, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* NET_XPORT_H */

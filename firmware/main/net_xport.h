/* net_xport.h — the transport seam. net.h stays the only API modes.c sees;
 * every net.h entry point that touches the MQTT session dispatches, inside
 * net.cpp, through the `net_xport_ops_t` returned by xport_lte_ops(). This
 * header is that vtable's shape — consumed only by net.cpp (the dispatcher)
 * and xport_lte.cpp (the one implementation); modes.c never includes it.
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

    /* net_liveness_ping_now() (LOCATION_TRACKING_DESIGN.md §4 item 4, task
     * F2). */
    void (*ping_now)(void);
} net_xport_ops_t;

/* Implemented in xport_lte.cpp: returns the ops table wired to the moved,
 * unmodified LTE MQTT session code (net_session_up/down,
 * pager_mqtt_event_handler, net_service_session, etc. under this vtable's
 * field names -- see that file's own module comment). net_init() registers
 * this as the (only) transport. */
const net_xport_ops_t *xport_lte_ops(void);

/* net.cpp's own trampoline to the single callback registered via
 * net_set_uplink_window_cb() (net.h), for xport_lte.cpp. No-op if no callback is registered
 * yet. LOCATION_TRACKING_DESIGN.md task F2. */
void net_dispatch_uplink_window(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_XPORT_H */

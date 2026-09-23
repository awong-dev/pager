// xport_wifi.c — the WiFi MQTT transport (docs/WIFI_DESIGN.md §1, §5;
// docs/WIFI_TASKS.md W5). esp-mqtt over esp-tls/mbedtls on the STA
// wifi_sta.c brings up. Mirrors xport_lte.cpp's event-handler semantics
// (net.cpp:327-500's pre-move code) wherever the two protocols allow the
// same shape; every place this differs from LTE is commented with why.
//
// Plain C: esp-mqtt/esp-wifi/esp-tls are all C APIs (unlike WalterModem,
// nothing here forces C++).
//
// Authority: docs/WIFI_DESIGN.md §1 (the event-mapping table), §5 (TLS).

#include "net.h"
#include "net_xport.h"
#include "net_connect_guard.h"
#include "publish_quiet.h"
#include "ident.h"
#include "catrust.h"

#include "mqtt_client.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";

// PROTOCOL.md §3.3: same hard envelope limit as the LTE transport
// (xport_lte.cpp's own PAGER_MAX_PAYLOAD). WiFi never carries the
// pager/boot/... bootstrap namespace (setup.c's bootstrap hop is LTE-only,
// docs/WIFI_DESIGN.md §1's "stay in net.cpp, LTE-only" list), so there is
// only ever the one cap here, unlike xport_lte.cpp's topic-aware two caps.
#define PAGER_MAX_PAYLOAD 640u

// docs/WIFI_DESIGN.md §1's esp-mqtt equivalents table: real PINGREQs at a
// keepalive short enough to survive a home NAT.
#define WIFI_MQTT_KEEPALIVE_S 60

// docs/WIFI_TASKS.md W5's own buffer.size = 1024: a 640-byte envelope is
// never split into multiple MQTT_EVENT_DATA fragments.
#define WIFI_MQTT_BUFFER_SIZE 1024

static esp_mqtt_client_handle_t s_client = NULL;
static char s_down_topic[48] = "";

static volatile bool s_mqtt_connected = false;
static volatile bool s_disconnect_edge = false;
static volatile int s_last_rc = 0;
static volatile net_mqtt_rc_class_t s_last_class = NET_MQTT_RC_NONE;
static volatile int64_t s_last_uplink_us = 0;
static volatile uint32_t s_oversize_count = 0;

static net_connect_guard_t s_connect_guard;
static publish_quiet_gate_t s_publish_quiet;
static bool s_guards_inited = false;

static void wifi_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                     void *event_data)
{
    (void) handler_args;
    (void) base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;

    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        // docs/WIFI_DESIGN.md §1's event-mapping table: subscribe from here,
        // same "connected is not usable, SUBSCRIBED is" rule as LTE --
        // s_mqtt_connected is only set on MQTT_EVENT_SUBSCRIBED below.
        ESP_LOGI(TAG, "MQTT connected, resubscribing to %s", s_down_topic);
        esp_mqtt_client_subscribe(event->client, s_down_topic, 1);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        net_connect_guard_clear(&s_connect_guard);
        if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
            // SUBACK carried 0x80 for our one topic (e.g. ACL denial) --
            // esp-mqtt still raises SUBSCRIBED, just with this flagged.
            ESP_LOGI(TAG, "MQTT subscribe failed (broker refused, SUBACK 0x80)");
            break;
        }
        s_mqtt_connected = true;
        s_last_uplink_us = esp_timer_get_time();
        ESP_LOGI(TAG, "MQTT session usable (subscribed to '%s')", s_down_topic);
        break;

    case MQTT_EVENT_PUBLISHED:
        // Only ever fires for QoS 1/2 (PUBACK/PUBCOMP) -- mqtt_client.c's own
        // deliver_puback()/PUBCOMP handling. QoS 0 publishes close their own
        // publish_quiet_gate_done() synchronously in wifi_publish_common()
        // below (see that function's own comment for why).
        s_last_uplink_us = esp_timer_get_time();
        publish_quiet_gate_done(&s_publish_quiet, esp_timer_get_time());
        break;

    case MQTT_EVENT_DATA:
        // Header's own contract (mqtt_client.h): only the FIRST event of a
        // message carries `topic`; a message that arrives in more than one
        // MQTT_EVENT_DATA event (total_data_len != data_len on the first
        // one) means it is bigger than buffer.size -- with
        // WIFI_MQTT_BUFFER_SIZE=1024 that is unreachable for a <=640B
        // envelope in practice, but treat it exactly like xport_lte.cpp's
        // oversize path anyway: count and drop, never render a partial
        // message.
        if (event->topic == NULL) {
            // A continuation fragment of an already-counted oversize
            // message (or, defensively, a stray event with none) -- nothing
            // more to do with it.
            break;
        }
        if (event->total_data_len != event->data_len || (unsigned) event->data_len > PAGER_MAX_PAYLOAD) {
            s_oversize_count = s_oversize_count + 1;
            ESP_LOGI(TAG, "oversize MQTT message dropped: %d bytes (total %d) > %u cap",
                     event->data_len, event->total_data_len, (unsigned) PAGER_MAX_PAYLOAD);
            break;
        }
        {
            char topic[64];
            size_t tlen = (size_t) event->topic_len < sizeof(topic) - 1 ? (size_t) event->topic_len
                                                                         : sizeof(topic) - 1;
            memcpy(topic, event->topic, tlen);
            topic[tlen] = '\0';
            // net_internal.h's own s_msg_cb is off-limits here by that
            // header's explicit module comment ("never a future
            // xport_wifi.c") -- net_dispatch_msg() (net.cpp, net_xport.h) is
            // the trampoline both transports are meant to go through.
            net_dispatch_msg(topic, event->data, (uint16_t) event->data_len);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        net_connect_guard_clear(&s_connect_guard);
        s_mqtt_connected = false;
        s_disconnect_edge = true;
        ESP_LOGI(TAG, "MQTT disconnected, rc=%d", s_last_rc);
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            switch (event->error_handle->error_type) {
            case MQTT_ERROR_TYPE_CONNECTION_REFUSED: {
                int rc = event->error_handle->connect_return_code;
                s_last_rc = rc;
                // docs/WIFI_DESIGN.md §1's table: 0x01/0x02/0x03 transient,
                // 0x04 (bad username/password)/0x05 (not authorized)
                // permanent, mirroring xport_lte.cpp's classify_mqtt_rc().
                s_last_class = (rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                                rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)
                                   ? NET_MQTT_RC_PERMANENT
                                   : NET_MQTT_RC_TRANSIENT;
                ESP_LOGI(TAG, "MQTT connect failed, rc=%d", rc);
                break;
            }
            case MQTT_ERROR_TYPE_TCP_TRANSPORT:
                if (event->error_handle->esp_tls_stack_err != 0 ||
                    event->error_handle->esp_tls_cert_verify_flags != 0) {
                    s_last_class = NET_MQTT_RC_TLS_FAIL;
                    // docs/WIFI_DESIGN.md §5: reported/logged/counted only --
                    // NEVER calls catrust_on_mqtt_tls_fail(). That state
                    // machine's job is deciding whether to keep validating
                    // on LTE; a WiFi TLS failure is not evidence about the
                    // modem, and WiFi always has LTE to fall back to (§5's
                    // "an unverifiable broker is a reason to switch
                    // transport, not to lower the bar").
                    ESP_LOGI(TAG,
                             "MQTT TLS failure (WiFi transport, reported only): "
                             "stack_err=%d verify_flags=0x%x",
                             event->error_handle->esp_tls_stack_err,
                             event->error_handle->esp_tls_cert_verify_flags);
                } else {
                    s_last_class = NET_MQTT_RC_TRANSIENT;
                    ESP_LOGI(TAG, "MQTT transport error: esp_err=0x%x sock_errno=%d",
                             event->error_handle->esp_tls_last_esp_err,
                             event->error_handle->esp_transport_sock_errno);
                }
                break;
            default:
                s_last_class = NET_MQTT_RC_TRANSIENT;
                break;
            }
        }
        break;

    default:
        break;
    }
}

static bool wifi_up(void)
{
    if (!s_guards_inited) {
        net_connect_guard_init(&s_connect_guard);
        publish_quiet_gate_init(&s_publish_quiet);
        s_guards_inited = true;
    }
    if (s_client) {
        // net_xport_switch() never calls .up() on an already-active
        // transport; defensive only.
        return true;
    }
    // docs/WIFI_DESIGN.md §5: mandatory, no plaintext fallback, ever.
    if (catrust_get_state() != CATRUST_PINNED) {
        ESP_LOGI(TAG, "WiFi MQTT refused: CA not pinned (docs/WIFI_DESIGN.md §5)");
        return false;
    }

    snprintf(s_down_topic, sizeof(s_down_topic), "pager/%s/down", ident_get_dev_id());
    s_disconnect_edge = false;

    esp_mqtt_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.broker.address.hostname = ident_get_host();
    cfg.broker.address.port = ident_get_port();
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    // ident_get_ca() is a NUL-terminated PEM (ident.h); certificate_len=0
    // per mqtt_client.h's own "PEM format must have a terminating NULL
    // character and the related len field set to 0" contract.
    cfg.broker.verification.certificate = ident_get_ca();
    cfg.credentials.username = ident_get_dev_id();
    cfg.credentials.client_id = ident_get_dev_id();
    cfg.credentials.authentication.password = ident_get_mqtt_pw();
    cfg.session.keepalive = WIFI_MQTT_KEEPALIVE_S;
    // docs/WIFI_DESIGN.md §1: no LWT in phase 1 (an LWT from a dropped WiFi
    // session could overwrite a newer retained `online` published from
    // LTE; making that safe needs a relay-side ordering rule that is not
    // part of this design). cfg.session.last_will is left zeroed.
    //
    // NOTE (deviation from docs/WIFI_TASKS.md W5's literal text): the spec
    // names this field `session.disable_auto_reconnect`. In this IDF's
    // esp-mqtt (mqtt_client.h, esp_mqtt_client_config_t), the field actually
    // lives under `.network`, not `.session` -- verified by reading the
    // vendored header on this machine. Set below, under the field that
    // actually exists and compiles.
    cfg.network.disable_auto_reconnect = true;
    cfg.buffer.size = WIFI_MQTT_BUFFER_SIZE;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGI(TAG, "esp_mqtt_client_init() failed");
        net_connect_guard_note_fail(&s_connect_guard);
        return false;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, wifi_mqtt_event_handler, NULL);

    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGI(TAG, "esp_mqtt_client_start() failed");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        net_connect_guard_note_fail(&s_connect_guard);
        return false;
    }
    net_connect_guard_issued(&s_connect_guard, esp_timer_get_time());
    ESP_LOGI(TAG, "MQTT connect issued to %s:%u", ident_get_host(), (unsigned) ident_get_port());
    return true;
}

static void wifi_down(void)
{
    if (!s_client) {
        return;
    }
    // esp_mqtt_client_stop()/_destroy() are blocking calls on the caller's
    // task (never the event handler's -- mqtt_client.h's own doc comment);
    // net_xport_switch() (net.cpp) calls .down() from whichever task issued
    // the switch (the console task for every path this phase-1 task has),
    // never from wifi_mqtt_event_handler() above.
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL; // docs/WIFI_DESIGN.md §2: "assert no client object remains"
    s_mqtt_connected = false;
    net_connect_guard_clear(&s_connect_guard);
}

// docs/WIFI_DESIGN.md §1's oversize/publish rows + this file's own
// esp_mqtt_client_publish() QoS-0 note below. Shared by wifi_publish()/
// wifi_publish_raw() -- the only difference between the two on the LTE side
// was the buffer's constness, which esp_mqtt_client_publish()'s `const
// char *data` parameter erases anyway (binary-safe: the length is passed
// explicitly, esp-mqtt never relies on a NUL terminator for the payload).
static bool wifi_publish_common(const char *topic, const void *buf, uint16_t len, uint8_t qos)
{
    if (len > PAGER_MAX_PAYLOAD) {
        ESP_LOGI(TAG, "refusing to publish %u bytes > %u cap (PROTOCOL.md §3.3)", (unsigned) len,
                 (unsigned) PAGER_MAX_PAYLOAD);
        return false;
    }
    if (!s_client) {
        ESP_LOGI(TAG, "WiFi MQTT publish refused: no session up");
        return false;
    }
    int64_t now = esp_timer_get_time();
    publish_quiet_gate_issued(&s_publish_quiet, now);
    int msg_id = esp_mqtt_client_publish(s_client, topic, (const char *) buf, (int) len, qos, 0);
    if (msg_id < 0) {
        // The round trip never started (outbox full or client not
        // connected) -- release the gate immediately rather than waiting on
        // an event that will never come.
        publish_quiet_gate_done(&s_publish_quiet, esp_timer_get_time());
        ESP_LOGI(TAG, "WiFi MQTT publish failed (msg_id=%d)", msg_id);
        return false;
    }
    s_last_uplink_us = now;
    if (qos == 0) {
        // esp-mqtt's own doc comment (mqtt_client.h, esp_mqtt_client_publish()):
        // "sends the publish message immediately in the user task's
        // context". Unlike QoS 1/2, there is no PUBACK/PUBCOMP to wait for,
        // and mqtt_client.c only raises MQTT_EVENT_PUBLISHED from those two
        // handlers (never for QoS 0) -- so a QoS-0 publish's
        // publish_quiet_gate_issued() above would otherwise never get a
        // matching _done(), latching the panel-write quiet gate
        // (publish_quiet.h) shut forever after the first one. loc.c's own
        // unsolicited /loc publishes use qos 0 (PROTOCOL.md §13.1), so this
        // is a real path, not a hypothetical one. Close the gate here
        // instead, the instant the call above returns.
        publish_quiet_gate_done(&s_publish_quiet, esp_timer_get_time());
    }
    return true;
}

static bool wifi_publish(const char *topic, char *buf, uint16_t len, uint8_t qos)
{
    return wifi_publish_common(topic, buf, len, qos);
}

static bool wifi_publish_raw(const char *topic, uint8_t *buf, uint16_t len, uint8_t qos)
{
    return wifi_publish_common(topic, buf, len, qos);
}

static uint32_t wifi_publish_quiet_wait_ms(uint32_t max_wait_ms)
{
    uint32_t waited_ms = 0;
    while (publish_quiet_gate_should_wait(&s_publish_quiet, esp_timer_get_time())) {
        if (waited_ms >= max_wait_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // NEVER a tight busy-loop
        waited_ms += 10;
    }
    return waited_ms;
}

// docs/WIFI_DESIGN.md §1: "service() on WiFi only runs the policy tick and
// the connect timeout" -- no wifi_policy wiring exists yet in this
// phase-1/manual-only task (docs/WIFI_TASKS.md W5 does not touch
// wifi_policy.c), so this is just the connect timeout, mirroring
// xport_lte.cpp's M1 bound (net_connect_guard.h).
static void wifi_service_session(void)
{
    if (!s_client) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (net_connect_guard_check_timeout(&s_connect_guard, now)) {
        ESP_LOGI(TAG, "WiFi MQTT connect watchdog: no CONNECTED/SUBSCRIBED within %llds - "
                      "treating the session as dead",
                 (long long) (NET_CONNECT_TIMEOUT_US / 1000000));
        wifi_down();
        s_last_class = NET_MQTT_RC_TRANSIENT;
        s_disconnect_edge = true;
    }
}

static void wifi_get_mqtt_status(net_mqtt_status_t *out)
{
    if (!out) {
        return;
    }
    out->mqtt_connected = s_mqtt_connected;
    out->disconnect_edge = s_disconnect_edge;
    out->last_rc = s_last_rc;
    out->last_class = s_last_class;
    out->session_restart_edge = false; // no silent-resume concept on WiFi (esp-mqtt never resumes
                                        // silently: disable_auto_reconnect=true)
}

static void wifi_ack_disconnect_edge(void)
{
    s_disconnect_edge = false;
}

static void wifi_ack_session_restart_edge(void)
{
    // Never set (see wifi_get_mqtt_status()); nothing to clear.
}

static bool wifi_modem_busy(void)
{
    // No modem/UART/RTS interlock exists for this transport at all -- the
    // WiFi MQTT session never touches the Sequans modem (docs/WIFI_DESIGN.md
    // §1: "WiFi never touches the modem").
    return false;
}

static bool wifi_connect_in_flight(void)
{
    return net_connect_guard_in_flight(&s_connect_guard);
}

static bool wifi_connect_fail_streak_maxed(void)
{
    return net_connect_guard_should_escalate(&s_connect_guard);
}

static uint32_t wifi_take_memfull_delta(void)
{
    // No modem-buffer-memory-full equivalent exists for esp-mqtt's own
    // outbox in this design; always 0.
    return 0;
}

static uint32_t wifi_take_oversize_delta(void)
{
    uint32_t v = s_oversize_count;
    s_oversize_count = 0;
    return v;
}

static const net_xport_ops_t s_wifi_ops = {
    .up = wifi_up,
    .down = wifi_down,
    .publish = wifi_publish,
    .publish_raw = wifi_publish_raw,
    .publish_quiet_wait_ms = wifi_publish_quiet_wait_ms,
    .service = wifi_service_session,
    .status = wifi_get_mqtt_status,
    .ack_disconnect_edge = wifi_ack_disconnect_edge,
    .ack_session_restart_edge = wifi_ack_session_restart_edge,
    .modem_busy = wifi_modem_busy,
    .connect_in_flight = wifi_connect_in_flight,
    .connect_fail_streak_maxed = wifi_connect_fail_streak_maxed,
    .take_memfull_delta = wifi_take_memfull_delta,
    .take_oversize_delta = wifi_take_oversize_delta,
};

const net_xport_ops_t *xport_wifi_ops(void)
{
    return &s_wifi_ops;
}

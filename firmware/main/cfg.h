/* cfg.h — `kind:"cfg"` dispatcher (docs/V02_DESIGN.md §4.4, §7; docs/PROTOCOL.md
 * §3.2/§10).
 *
 * A single `cfg` push can now carry more than one sub-map (`lock`=0,
 * existing; `ca`=1; `sms`=2; `wifi`=3, docs/WIFI_DESIGN.md §4/docs/WIFI_TASKS.md
 * W3) in one envelope, and each sub-map has its own apply/ack timing (`lock`/
 * `sms`/`wifi` all apply and ack immediately; `ca`'s two-phase apply,
 * catrust.c, may defer the ack for several modes_run() cycles or never ack
 * at all if the apply is rejected). That ruled out the old design, where
 * lock_ingest_cfg_cbor() decoded the *whole* envelope itself and returned
 * true/false for "was this a cfg message" — a single push carrying both
 * `lock` and `ca` would have been entirely swallowed by whichever consumer
 * ran first. This module decodes the envelope exactly once, extracts the raw
 * CBOR byte span of every sub-map it recognises, and hands each span to its
 * own owning module (lock.c / catrust.c / sms.c / wificred.c) — teaching
 * none of those modules about each other, or about `cfg`'s own envelope
 * shape.
 *
 * Split the usual way: `cfg_parse()` (pure, no ESP-IDF dependency,
 * host-tested by firmware/host/test_cfg.c) does the byte-span extraction;
 * `cfg_ingest_cbor()` (device-only) is the one modes.c calls from
 * on_incoming_message(), in the same interception slot the old
 * lock_ingest_cfg_cbor() call used to occupy.
 */
#ifndef CFG_H
#define CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_ID_MAX 17 /* 16 chars + NUL, PROTOCOL.md §1 */

/* Byte spans are offsets into the SAME `buf`/`len` the caller passed to
 * cfg_parse() — the raw CBOR bytes of the sub-map value itself, starting at
 * its own map header, exactly what lock.c's/catrust.c's own cbor_r_init()
 * needs to decode it standalone. */
typedef struct {
    char id[CFG_ID_MAX];

    bool have_lock;
    size_t lock_off, lock_len;

    bool have_ca;
    size_t ca_off, ca_len;

    /* `sms`=2 (docs/V02_DESIGN.md §7, a later task): recognised as a KNOWN
     * key (so it is never treated as unknown-and-skipped-silently in a way
     * that would be indistinguishable from a real unknown key in a log), but
     * not yet consumed — cfg_ingest_cbor() does nothing with this span. */
    bool have_sms;
    size_t sms_off, sms_len;

    /* `wifi`=3 (docs/WIFI_DESIGN.md §4, docs/WIFI_TASKS.md W3): the WiFi
     * `{en, nets}` sub-map. Dispatched to wificred_apply_cfg_submap(). */
    bool have_wifi;
    size_t wifi_off, wifi_len;
} cfg_dispatch_t;

/* Decodes `buf`/`len` as a `/down` envelope already reduced to `count` map
 * pairs the same way msg.c's/lock.c's/loc.c's own readers do
 * (`sig_pair_present` stands in for `ident_get_flags() & IDENT_FLAG_REQ_SIG`,
 * same host-testability convention as lock_parse_cfg()/loc_parse_req_cbor()).
 * `buf` MUST already have passed auth_verify() when signed (the trailing
 * `sig` pair trimmed, the map header's own declared pair count left
 * untouched).
 *
 * Returns false if `buf` does not decode as a well-formed envelope with
 * `kind:"cfg"` — covering both "not cfg at all" and "cfg but malformed",
 * same deliberate conflation lock_parse_cfg()/loc_parse_req_cbor() already
 * document: either way the caller falls through to the normal ingest path.
 * A `cfg` envelope whose `cfg` map is empty, or contains only unknown keys,
 * is NOT malformed — "unknown cfg keys must be skipped, not treated as
 * malformed" (docs/V02_DESIGN.md §4.4) — and still returns true with every
 * `have_*` flag false. */
bool cfg_parse(const uint8_t *buf, uint16_t len, bool sig_pair_present, cfg_dispatch_t *out);

#ifdef ESP_PLATFORM
/* `cfg` intercept (docs/PROTOCOL.md §3.2/§10). Called from modes.c's
 * on_incoming_message(), after auth_verify(), before msg_ingest_down_cbor()
 * — same slot the old lock_ingest_cfg_cbor() call occupied. Returns true iff
 * `buf` decoded as `kind:"cfg"` (regardless of which, if any, sub-maps it
 * recognised) — caller MUST NOT also pass it to msg_ingest_down_cbor()
 * (a `cfg` envelope has neither `from` nor `body`, which that parser
 * requires). Dispatches `lock`'s span to lock_apply_cfg_submap() (applies +
 * acks `shown` immediately, unconditionally of lock state, matching the
 * pre-existing behaviour) and `ca`'s span to catrust_apply_cfg_submap()
 * (records the request only — the two-phase apply itself runs from
 * catrust_service(), modes_run()'s own task, never from this MQTT-event-task
 * call). `sms`'s span goes to sms_apply_cfg_submap() and `wifi`'s span
 * (docs/WIFI_TASKS.md W3) goes to wificred_apply_cfg_submap() — both apply +
 * ack `shown` immediately, same timing as `lock`. No modem or sleep-state
 * effect of its own beyond whatever lock.c's/catrust.c's/sms.c's/
 * wificred.c's own handlers already document. */
bool cfg_ingest_cbor(const uint8_t *buf, uint16_t len);
#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* CFG_H */

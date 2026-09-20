/* cfg.c — see cfg.h for the module split and every function's doc comment. */
#include "cfg.h"

#include <string.h>

#include "cbor.h"

/* PROTOCOL.md §10 envelope keymap subset this file reads, plus
 * relay/app/wirecbor.py's CFG_KEYMAP={"lock":0,"ca":1,"sms":2} sub-map keys
 * (cross-checked the same way lock.c's own CFGK_ constants comment already
 * documents). */
#define CFGK_ID 1
#define CFGK_KIND 6
#define CFGK_CFG 38
#define CFG_KEY_LOCK 0
#define CFG_KEY_CA 1
#define CFG_KEY_SMS 2

/* Records the current position as the start of a value, skips it (recursing
 * through nested maps/arrays as needed), and reports the [start,len) span —
 * this is what lets cfg_parse() hand lock.c/catrust.c the raw CBOR bytes of
 * their own sub-map without this file needing to know their shapes. */
static bool skip_capture(cbor_r_t *r, size_t *off, size_t *len)
{
    *off = r->pos;
    if (!cbor_r_skip(r)) {
        return false;
    }
    *len = r->pos - *off;
    return true;
}

static bool parse_cfg_submap(cbor_r_t *r, cfg_dispatch_t *out)
{
    uint32_t count;
    if (!cbor_r_map(r, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(r, &key)) {
            return false;
        }
        switch (key) {
        case CFG_KEY_LOCK:
            if (!skip_capture(r, &out->lock_off, &out->lock_len)) {
                return false;
            }
            out->have_lock = true;
            break;
        case CFG_KEY_CA:
            if (!skip_capture(r, &out->ca_off, &out->ca_len)) {
                return false;
            }
            out->have_ca = true;
            break;
        case CFG_KEY_SMS:
            if (!skip_capture(r, &out->sms_off, &out->sms_len)) {
                return false;
            }
            out->have_sms = true;
            break;
        default:
            /* "unknown cfg keys must be skipped, not treated as malformed"
             * (V02_DESIGN.md §4.4) — cbor_r_skip() handles any type. */
            if (!cbor_r_skip(r)) {
                return false;
            }
            break;
        }
    }
    return true;
}

bool cfg_parse(const uint8_t *buf, uint16_t len, bool sig_pair_present, cfg_dispatch_t *out)
{
    memset(out, 0, sizeof(*out));

    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count) || count == 0) {
        return false;
    }
    if (sig_pair_present) {
        /* docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule: auth_verify()
         * trims the trailing `sig` pair's bytes without rewriting this map
         * header's declared pair count — same adjustment lock.c's/loc.c's own
         * parsers make. */
        count -= 1;
    }

    bool is_cfg = false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        switch (key) {
        case CFGK_ID: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                return false;
            }
            if (slen < sizeof(out->id)) {
                memcpy(out->id, s, slen);
                out->id[slen] = '\0';
            }
            break;
        }
        case CFGK_KIND: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                return false;
            }
            is_cfg = (slen == 3 && memcmp(s, "cfg", 3) == 0);
            break;
        }
        case CFGK_CFG:
            if (!parse_cfg_submap(&r, out)) {
                return false;
            }
            break;
        default:
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }

    return is_cfg;
}

#ifdef ESP_PLATFORM

#include "catrust.h"
#include "ident.h"
#include "lock.h"
#include "msg.h"
#include "sms.h"

bool cfg_ingest_cbor(const uint8_t *buf, uint16_t len)
{
    bool sig_present = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    cfg_dispatch_t d;
    if (!cfg_parse(buf, len, sig_present, &d)) {
        return false; /* not cfg, or malformed cfg — caller falls through to msg.c's own ingest */
    }

    if (d.have_lock) {
        lock_apply_cfg_submap(buf + d.lock_off, (uint16_t) d.lock_len, d.id);
    }
    if (d.have_ca) {
        catrust_apply_cfg_submap(buf + d.ca_off, (uint16_t) d.ca_len, d.id);
    }
    if (d.have_sms) {
        /* v0.2 §6 (device-direct SMS): applies + acks `shown` immediately
         * (see sms_apply_cfg_submap()'s own doc comment) — same
         * immediate-apply-and-ack timing `cfg.lock` already uses. */
        sms_apply_cfg_submap(buf + d.sms_off, (uint16_t) d.sms_len, d.id);
    }

    return true;
}

#endif /* ESP_PLATFORM */

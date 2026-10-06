/* otapipe.c — see otapipe.h. */
#include "otapipe.h"

#include <string.h>

/* The PSRAM block must stay small next to the ~2 MB pool (docs/OTA_DESIGN.md section 3). The ROM tinfl
 * state (10,992 B, measured at build) is larger than the host's vendored copy, so the device total is
 * 49,696 B and the host test's 48 KiB bound does not apply here: this guards the device number at 56 KiB. */
_Static_assert(sizeof(otapipe_t) < 56 * 1024, "otapipe_t must stay under 56 KiB");

size_t otapipe_sizeof(void)
{
    return sizeof(otapipe_t);
}

/* Final image bytes -> stage -> write callback in OTAPIPE_STAGE pieces. */
static int stage_put(otapipe_t *p, const uint8_t *data, size_t n)
{
    if (p->out_limit && p->out_total + n > p->out_limit) {
        return -1;
    }
    p->out_total += n;
    while (n > 0) {
        size_t room = OTAPIPE_STAGE - p->stage_len;
        size_t c = n < room ? n : room;
        memcpy(p->stage + p->stage_len, data, c);
        p->stage_len += c;
        data += c;
        n -= c;
        if (p->stage_len == OTAPIPE_STAGE) {
            if (p->write(p->write_arg, p->stage, p->stage_len) != 0) {
                return -2;
            }
            p->stage_len = 0;
        }
    }
    return 0;
}

static int ap_from_read(void *arg, uint8_t *buf, size_t n)
{
    otapipe_t *p = (otapipe_t *) arg;
    if (n > p->base_len - p->base_pos) { /* base_pos <= base_len always */
        return -1;
    }
    memcpy(buf, p->base + p->base_pos, n);
    p->base_pos += n;
    return 0;
}

static int ap_from_seek(void *arg, int off)
{
    otapipe_t *p = (otapipe_t *) arg;
    int64_t np = (int64_t) p->base_pos + (int64_t) off;
    if (np < 0 || np > (int64_t) p->base_len) {
        return -1;
    }
    p->base_pos = (size_t) np;
    return 0;
}

static int ap_to_write(void *arg, const uint8_t *buf, size_t n)
{
    return stage_put((otapipe_t *) arg, buf, n);
}

int otapipe_init(otapipe_t *p, bool delta, size_t psz, const uint8_t *base, size_t base_len,
                 otapipe_write_fn w, void *arg)
{
    if (!p || !w || (delta && !base)) {
        return -1;
    }
    memset(p, 0, sizeof(*p));
    p->delta = delta;
    p->base = base;
    p->base_len = base_len;
    p->write = w;
    p->write_arg = arg;
    p->inflated_limit = psz;
    tinfl_init(&p->inf);
    mbedtls_sha256_init(&p->sha);
    if (mbedtls_sha256_starts(&p->sha, 0) != 0) {
        return -2;
    }
    if (delta) {
        if (detools_apply_patch_init(&p->ap, ap_from_read, ap_from_seek, psz, ap_to_write, p) != 0) {
            return -3;
        }
    }
    return 0;
}

void otapipe_deinit(otapipe_t *p)
{
    mbedtls_sha256_free(&p->sha);
}

void otapipe_set_out_limit(otapipe_t *p, size_t isz)
{
    p->out_limit = isz;
}

bool otapipe_push(otapipe_t *p, const uint8_t *in, size_t len)
{
    size_t unconsumed = p->in_len - p->in_off;
    if (unconsumed + len > OTAPIPE_IN_CAP) {
        return false;
    }
    if (p->in_off > 0) {
        memmove(p->in, p->in + p->in_off, unconsumed);
        p->in_len = unconsumed;
        p->in_off = 0;
    }
    memcpy(p->in + p->in_len, in, len);
    p->in_len += len;
    p->in_total += len;
    mbedtls_sha256_update(&p->sha, in, len);
    return true;
}

void otapipe_set_eof(otapipe_t *p)
{
    p->eof = true;
}

size_t otapipe_pending(const otapipe_t *p)
{
    if (p->failed || p->done) {
        return 0;
    }
    return (p->in_len - p->in_off) + (p->more_out ? 1 : 0) + (p->ob_len ? 1 : 0) + (p->inf_done ? 1 : 0);
}

/* Hands n inflated bytes at dict[ob_off] to the patcher / the stage. */
static int consume(otapipe_t *p, size_t n)
{
    const uint8_t *o = p->dict + p->ob_off;
    int r = p->delta ? detools_apply_patch_process(&p->ap, o, n) : stage_put(p, o, n);
    p->ob_off += n;
    p->ob_len -= n;
    return r;
}

int otapipe_drain(otapipe_t *p, size_t out_cap)
{
    if (p->failed) {
        return -1;
    }
    if (p->done) {
        return 1;
    }
    size_t produced = 0;
    for (;;) {
        /* 1. Flush inflated bytes still waiting in the dictionary window, at most out_cap per call.
         * (tinfl cannot be told to stop short of the end of its 32 KB window, so the cap is applied
         * here, on the way to the flash.) */
        if (p->ob_len) {
            if (produced >= out_cap) {
                break;
            }
            size_t n = p->ob_len < out_cap - produced ? p->ob_len : out_cap - produced;
            if (consume(p, n) != 0) {
                p->failed = true;
                return -4;
            }
            produced += n;
            continue;
        }
        if (p->inf_done) {
            p->done = true;
            return 1;
        }
        if (produced >= out_cap) {
            break;
        }
        size_t avail = p->in_len - p->in_off;
        if (avail == 0 && !p->more_out && !p->eof) {
            break; /* needs input */
        }
        size_t ilen = avail;
        size_t olen = TINFL_LZ_DICT_SIZE - p->dpos; /* must reach the window end: tinfl wraps by mask */
        mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (p->eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        int st = (int) tinfl_decompress(&p->inf, p->in + p->in_off, &ilen, p->dict, p->dict + p->dpos,
                                        &olen, flags);
        p->in_off += ilen;
        if (p->in_off == p->in_len) {
            p->in_off = p->in_len = 0;
        }
        if (st < 0) {
            p->failed = true;
            return -2;
        }
        if (olen > 0) {
            if (p->inflated_limit && p->inflated_total + olen > p->inflated_limit) {
                p->failed = true;
                return -3;
            }
            p->inflated_total += olen;
            p->ob_off = p->dpos;
            p->ob_len = olen;
            p->dpos = (p->dpos + olen) & (TINFL_LZ_DICT_SIZE - 1);
        }
        p->more_out = (st == TINFL_STATUS_HAS_MORE_OUTPUT);
        if (st == TINFL_STATUS_DONE) {
            p->inf_done = true;
        } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && p->in_len == p->in_off && !p->eof && olen == 0) {
            break;
        } else if (ilen == 0 && olen == 0 && st != TINFL_STATUS_HAS_MORE_OUTPUT) {
            break; /* no progress possible without new input */
        }
    }
    return 0;
}

int otapipe_finish(otapipe_t *p, uint8_t osha[32], uint32_t *out_len)
{
    if (p->failed || !p->done || p->in_len != p->in_off) {
        return -1;
    }
    if (p->delta) {
        if (detools_apply_patch_finalize(&p->ap) < 0) {
            p->failed = true;
            return -2;
        }
    }
    if (p->stage_len > 0) {
        if (p->write(p->write_arg, p->stage, p->stage_len) != 0) {
            p->failed = true;
            return -3;
        }
        p->stage_len = 0;
    }
    if (mbedtls_sha256_finish(&p->sha, osha) != 0) {
        return -4;
    }
    if (out_len) {
        *out_len = (uint32_t) p->out_total;
    }
    return 0;
}

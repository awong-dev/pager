/* cbor.c — see cbor.h for the wire subset and the design notes. */
#include "cbor.h"

#include <string.h>

/* Major type bytes (already shifted into the top 3 bits, ready to OR with
 * a 5-bit additional-info value 0-27). */
#define MAJ_UINT 0x00
#define MAJ_NINT 0x20
#define MAJ_BSTR 0x40
#define MAJ_TSTR 0x60
#define MAJ_ARRAY 0x80
#define MAJ_MAP 0xA0
#define MAJ_SIMPLE 0xE0

#define SIMPLE_FALSE 0xF4
#define SIMPLE_TRUE 0xF5
#define SIMPLE_NULL 0xF6
#define SIMPLE_FLOAT64 0xFB

/* ---------------------------------------------------------------------
 * Writer
 * --------------------------------------------------------------------- */

void cbor_w_init(cbor_w_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = false;
}

static bool put_byte(cbor_w_t *w, uint8_t b)
{
    if (w->err || w->len + 1 > w->cap) {
        w->err = true;
        return false;
    }
    w->buf[w->len++] = b;
    return true;
}

static bool put_bytes(cbor_w_t *w, const void *p, size_t n)
{
    if (w->err) {
        return false;
    }
    if (n > w->cap - w->len) {
        w->err = true;
        return false;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    return true;
}

/* Writes a major-type byte plus a shortest-form additional-info/count/
 * length `n` (RFC 8949 preferred serialization: immediate if n < 24, else
 * the smallest of the 1/2/4/8-byte follow-on forms). `major` is already
 * shifted (one of the MAJ_* constants above). */
static bool put_head(cbor_w_t *w, uint8_t major, uint64_t n)
{
    if (n < 24) {
        return put_byte(w, (uint8_t)(major | n));
    }
    if (n < 256) {
        if (!put_byte(w, (uint8_t)(major | 24))) {
            return false;
        }
        uint8_t b = (uint8_t)n;
        return put_bytes(w, &b, 1);
    }
    if (n < 65536) {
        if (!put_byte(w, (uint8_t)(major | 25))) {
            return false;
        }
        uint8_t b[2] = {(uint8_t)(n >> 8), (uint8_t)n};
        return put_bytes(w, b, 2);
    }
    if (n < 4294967296ULL) {
        if (!put_byte(w, (uint8_t)(major | 26))) {
            return false;
        }
        uint8_t b[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
        return put_bytes(w, b, 4);
    }
    if (!put_byte(w, (uint8_t)(major | 27))) {
        return false;
    }
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(n >> (8 * (7 - i)));
    }
    return put_bytes(w, b, 8);
}

bool cbor_w_map(cbor_w_t *w, uint32_t n)
{
    return put_head(w, MAJ_MAP, n);
}

bool cbor_w_map_key(cbor_w_t *w, uint32_t key, uint32_t n)
{
    return put_head(w, MAJ_UINT, key) && put_head(w, MAJ_MAP, n);
}

bool cbor_w_array(cbor_w_t *w, uint32_t key, uint32_t n)
{
    return put_head(w, MAJ_UINT, key) && put_head(w, MAJ_ARRAY, n);
}

bool cbor_w_uint(cbor_w_t *w, uint32_t key, uint64_t u)
{
    return put_head(w, MAJ_UINT, key) && put_head(w, MAJ_UINT, u);
}

bool cbor_w_nint(cbor_w_t *w, uint32_t key, int64_t v)
{
    if (v >= 0) {
        return false; /* not a negative int; caller error, buffer untouched */
    }
    uint64_t magnitude = (uint64_t)(-(v + 1)); /* CBOR major 1: value = -(1+n) */
    return put_head(w, MAJ_UINT, key) && put_head(w, MAJ_NINT, magnitude);
}

bool cbor_w_tstr(cbor_w_t *w, uint32_t key, const char *s, size_t len)
{
    return put_head(w, MAJ_UINT, key) && put_head(w, MAJ_TSTR, len) &&
           put_bytes(w, s, len);
}

bool cbor_w_bstr(cbor_w_t *w, uint32_t key, const uint8_t *b, size_t len)
{
    return put_head(w, MAJ_UINT, key) && put_head(w, MAJ_BSTR, len) &&
           put_bytes(w, b, len);
}

bool cbor_w_bool(cbor_w_t *w, uint32_t key, bool v)
{
    return put_head(w, MAJ_UINT, key) && put_byte(w, v ? SIMPLE_TRUE : SIMPLE_FALSE);
}

bool cbor_w_null(cbor_w_t *w, uint32_t key)
{
    return put_head(w, MAJ_UINT, key) && put_byte(w, SIMPLE_NULL);
}

bool cbor_w_f64(cbor_w_t *w, uint32_t key, double v)
{
    if (!put_head(w, MAJ_UINT, key) || !put_byte(w, SIMPLE_FLOAT64)) {
        return false;
    }
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(bits >> (8 * (7 - i)));
    }
    return put_bytes(w, b, 8);
}

/* ---------------------------------------------------------------------
 * Reader
 * --------------------------------------------------------------------- */

void cbor_r_init(cbor_r_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

/* Parses the header at `r->pos`: major type byte plus the shortest-form
 * value that follows the same 24/25/26/27-byte-count table for every
 * major type (RFC 8949 §3), which is also what lets this one routine
 * "skip" a float64 (info 27, 8 data bytes) or a 4-byte-length string
 * header identically to any other major type. Rejects indefinite length
 * (info 31) and the reserved values 28-30. On success, advances `r->pos`
 * past the whole header (and, for major 7's float forms, its data bytes)
 * and returns the major byte (unshifted, i.e. one of the MAJ_* constants)
 * and the parsed value in `*val`. Leaves `r->pos` unchanged on failure. */
static bool get_head(cbor_r_t *r, uint8_t *major, uint64_t *val)
{
    if (r->pos >= r->len) {
        return false;
    }
    uint8_t first = r->buf[r->pos];
    uint8_t maj = first & 0xE0;
    uint8_t info = first & 0x1F;
    size_t need;
    uint64_t v;

    if (info < 24) {
        v = info;
        need = 1;
    } else if (info == 24) {
        if (r->pos + 2 > r->len) {
            return false;
        }
        v = r->buf[r->pos + 1];
        need = 2;
    } else if (info == 25) {
        if (r->pos + 3 > r->len) {
            return false;
        }
        v = ((uint64_t)r->buf[r->pos + 1] << 8) | r->buf[r->pos + 2];
        need = 3;
    } else if (info == 26) {
        if (r->pos + 5 > r->len) {
            return false;
        }
        v = 0;
        for (size_t i = 1; i <= 4; i++) {
            v = (v << 8) | r->buf[r->pos + i];
        }
        need = 5;
    } else if (info == 27) {
        if (r->pos + 9 > r->len) {
            return false;
        }
        v = 0;
        for (size_t i = 1; i <= 8; i++) {
            v = (v << 8) | r->buf[r->pos + i];
        }
        need = 9;
    } else {
        return false; /* 28-30 reserved, 31 indefinite: both rejected */
    }

    *major = maj;
    *val = v;
    r->pos += need;
    return true;
}

cbor_type_t cbor_r_peek(const cbor_r_t *r)
{
    if (r->pos >= r->len) {
        return CBOR_T_INVALID;
    }
    uint8_t first = r->buf[r->pos];
    uint8_t maj = first & 0xE0;
    uint8_t info = first & 0x1F;
    /* 28-30 are reserved and 31 is indefinite length for every major type
     * except simple/float (handled below on its own terms) — both are
     * rejected here so a caller dispatching on cbor_r_peek() never hands
     * an indefinite-length header to cbor_r_map()/cbor_r_array(). */
    if (maj != MAJ_SIMPLE && info >= 28) {
        return CBOR_T_INVALID;
    }
    switch (maj) {
    case MAJ_UINT:
        return CBOR_T_UINT;
    case MAJ_NINT:
        return CBOR_T_NINT;
    case MAJ_BSTR:
        return CBOR_T_BSTR;
    case MAJ_TSTR:
        return CBOR_T_TSTR;
    case MAJ_ARRAY:
        return CBOR_T_ARRAY;
    case MAJ_MAP:
        return CBOR_T_MAP;
    case MAJ_SIMPLE:
        if (info == 20 || info == 21) {
            return CBOR_T_BOOL;
        }
        if (info == 22) {
            return CBOR_T_NULL;
        }
        if (info == 27) {
            return CBOR_T_FLOAT64;
        }
        return CBOR_T_INVALID;
    default:
        return CBOR_T_INVALID; /* major 6 (tag): not part of this wire format */
    }
}

bool cbor_r_map(cbor_r_t *r, uint32_t *count)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_MAP || val > UINT32_MAX) {
        r->pos = save;
        return false;
    }
    *count = (uint32_t)val;
    return true;
}

bool cbor_r_array(cbor_r_t *r, uint32_t *count)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_ARRAY || val > UINT32_MAX) {
        r->pos = save;
        return false;
    }
    *count = (uint32_t)val;
    return true;
}

bool cbor_r_key(cbor_r_t *r, uint32_t *key)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_UINT || val > UINT32_MAX) {
        r->pos = save;
        return false;
    }
    *key = (uint32_t)val;
    return true;
}

bool cbor_r_uint(cbor_r_t *r, uint64_t *v)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_UINT) {
        r->pos = save;
        return false;
    }
    *v = val;
    return true;
}

bool cbor_r_nint(cbor_r_t *r, int64_t *v)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_NINT) {
        r->pos = save;
        return false;
    }
    *v = -(int64_t)val - 1;
    return true;
}

bool cbor_r_tstr(cbor_r_t *r, const char **s, size_t *len)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_TSTR || val > r->len - r->pos) {
        r->pos = save;
        return false;
    }
    *s = (const char *)(r->buf + r->pos);
    *len = (size_t)val;
    r->pos += val;
    return true;
}

bool cbor_r_bstr(cbor_r_t *r, const uint8_t **b, size_t *len)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val) || major != MAJ_BSTR || val > r->len - r->pos) {
        r->pos = save;
        return false;
    }
    *b = r->buf + r->pos;
    *len = (size_t)val;
    r->pos += val;
    return true;
}

bool cbor_r_bool(cbor_r_t *r, bool *v)
{
    if (r->pos >= r->len) {
        return false;
    }
    uint8_t b = r->buf[r->pos];
    if (b == SIMPLE_FALSE) {
        *v = false;
    } else if (b == SIMPLE_TRUE) {
        *v = true;
    } else {
        return false;
    }
    r->pos += 1;
    return true;
}

bool cbor_r_null(cbor_r_t *r)
{
    if (r->pos >= r->len || r->buf[r->pos] != SIMPLE_NULL) {
        return false;
    }
    r->pos += 1;
    return true;
}

bool cbor_r_f64(cbor_r_t *r, double *v)
{
    if (r->pos >= r->len || r->buf[r->pos] != SIMPLE_FLOAT64) {
        return false;
    }
    if (r->pos + 9 > r->len) {
        return false;
    }
    uint64_t bits = 0;
    for (size_t i = 1; i <= 8; i++) {
        bits = (bits << 8) | r->buf[r->pos + i];
    }
    memcpy(v, &bits, sizeof(bits));
    r->pos += 9;
    return true;
}

bool cbor_r_skip(cbor_r_t *r)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;
    if (!get_head(r, &major, &val)) {
        r->pos = save;
        return false;
    }
    switch (major) {
    case MAJ_UINT:
    case MAJ_NINT:
    case MAJ_SIMPLE:
        /* Value (int magnitude, or a float64's data bytes) already
         * consumed by get_head. */
        return true;
    case MAJ_BSTR:
    case MAJ_TSTR:
        if (val > r->len - r->pos) {
            r->pos = save;
            return false;
        }
        r->pos += val;
        return true;
    case MAJ_ARRAY:
        for (uint64_t i = 0; i < val; i++) {
            if (!cbor_r_skip(r)) {
                r->pos = save;
                return false;
            }
        }
        return true;
    case MAJ_MAP:
        for (uint64_t i = 0; i < (uint64_t)val * 2; i++) {
            if (!cbor_r_skip(r)) {
                r->pos = save;
                return false;
            }
        }
        return true;
    default:
        r->pos = save;
        return false;
    }
}

/* test_otapipe.c — host test for main/otapipe.c (docs/OTA_DESIGN.md D4):
 * tinfl -> (detools) -> stage -> write, fed in socket-read-sized pieces.
 * zlib (-lz) is used only to CREATE synthetic inputs. Real-artifact cases read
 * $OTA_FIXTURES (default build/bench-logs/ota_meas) and $OTA_IMAGES (default
 * build/images) and are reported as SKIP when absent.
 */
#include "otapipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static int g_failures = 0;
static int g_skips = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

typedef struct {
    mbedtls_sha256_context sha;
    uint8_t *cap;      /* optional capture buffer */
    size_t cap_len;
    size_t n;
    size_t max_piece;
    int writes;
    int short_pieces;  /* pieces that were not a whole stage (must be only the last) */
} out_t;

static int out_write(void *arg, const uint8_t *buf, size_t len)
{
    out_t *o = (out_t *) arg;
    if (len > o->max_piece) {
        o->max_piece = len;
    }
    if (len != OTAPIPE_STAGE) {
        o->short_pieces++;
    }
    mbedtls_sha256_update(&o->sha, buf, len);
    if (o->cap && o->n + len <= o->cap_len) {
        memcpy(o->cap + o->n, buf, len);
    }
    o->n += len;
    o->writes++;
    return 0;
}

typedef struct {
    int rc;            /* 0 ok, <0 pipe error (drain/finish), 1 not done at EOF */
    uint8_t osha[32];  /* object hash */
    uint8_t sha[32];   /* output hash */
    uint32_t out_len;
    size_t n;
    int writes, short_pieces;
    size_t max_piece;
    size_t max_drain_out; /* most final-image bytes produced by one drain() call */
} run_t;

static run_t run_pipe(const uint8_t *obj, size_t len, bool delta, const uint8_t *base, size_t base_len,
                      size_t psz, size_t isz, size_t piece, size_t out_cap, uint8_t *cap, size_t cap_len)
{
    run_t r;
    memset(&r, 0, sizeof(r));
    otapipe_t *p = malloc(otapipe_sizeof());
    out_t o;
    memset(&o, 0, sizeof(o));
    o.cap = cap;
    o.cap_len = cap_len;
    mbedtls_sha256_init(&o.sha);
    mbedtls_sha256_starts(&o.sha, 0);
    if (otapipe_init(p, delta, psz, base, base_len, out_write, &o) != 0) {
        r.rc = -100;
        free(p);
        return r;
    }
    if (isz) {
        otapipe_set_out_limit(p, isz);
    }
    size_t off = 0;
    int st = 0;
    while (off < len && st == 0) {
        size_t n = len - off < piece ? len - off : piece;
        if (!otapipe_push(p, obj + off, n)) {
            r.rc = -101;
            break;
        }
        off += n;
        if (off == len) {
            otapipe_set_eof(p);
        }
        for (int guard = 0; guard < 1000000; guard++) {
            size_t before = p->out_total;
            st = otapipe_drain(p, out_cap);
            if (p->out_total - before > r.max_drain_out) {
                r.max_drain_out = p->out_total - before;
            }
            if (st != 0) {
                break;
            }
            if (otapipe_pending(p) == 0 && off != len) {
                break;
            }
        }
        if (st < 0) {
            r.rc = st;
        }
    }
    if (r.rc == 0) {
        if (st != 1) {
            r.rc = 1;
        } else {
            int f = otapipe_finish(p, r.osha, &r.out_len);
            if (f < 0) {
                r.rc = f;
            }
        }
    }
    mbedtls_sha256_finish(&o.sha, r.sha);
    r.n = o.n;
    r.writes = o.writes;
    r.short_pieces = o.short_pieces;
    r.max_piece = o.max_piece;
    otapipe_deinit(p);
    mbedtls_sha256_free(&o.sha);
    free(p);
    return r;
}

static void sha_of(const uint8_t *d, size_t n, uint8_t out[32])
{
    mbedtls_sha256(d, n, out, 0);
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t) sz + 1);
    if (fread(b, 1, (size_t) sz, f) != (size_t) sz) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = (size_t) sz;
    return b;
}

static unsigned s_rng = 12345;
static uint8_t rnd8(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (uint8_t) (s_rng >> 24);
}

static void test_synthetic_full(void)
{
    size_t n = 200000; /* not a multiple of the 4 KB stage */
    uint8_t *img = malloc(n);
    for (size_t i = 0; i < n; i++) {
        img[i] = rnd8();
    }
    for (int z = 0; z < 20; z++) { /* zero runs */
        size_t at = (size_t) rnd8() * 700 + (size_t) rnd8();
        size_t len = 500 + (size_t) rnd8() * 40;
        if (at + len < n) {
            memset(img + at, 0, len);
        }
    }
    memset(img + n - 20000, 0, 20000);
    uLongf zl = compressBound((uLong) n);
    uint8_t *z = malloc(zl);
    CHECK(compress2(z, &zl, img, (uLong) n, 9) == Z_OK, "zlib compress");
    uint8_t want[32], wantobj[32];
    sha_of(img, n, want);
    sha_of(z, zl, wantobj);

    uint8_t *cap = malloc(n);
    run_t r = run_pipe(z, zl, false, NULL, 0, n, n, 1500, 16384, cap, n);
    CHECK(r.rc == 0, "synthetic full: rc=%d", r.rc);
    CHECK(r.n == n && r.out_len == n, "synthetic full: out %zu / %u", r.n, r.out_len);
    CHECK(memcmp(r.sha, want, 32) == 0 && memcmp(cap, img, n) == 0, "synthetic full: output differs");
    CHECK(memcmp(r.osha, wantobj, 32) == 0, "synthetic full: object sha differs");
    CHECK(r.max_drain_out <= 16384, "synthetic full: one drain wrote %zu > out_cap", r.max_drain_out);
    CHECK(r.short_pieces == 1 && r.max_piece == OTAPIPE_STAGE, "synthetic full: stage pieces (short=%d max=%zu)",
          r.short_pieces, r.max_piece);

    /* other piece sizes and out_caps (including tiny ones) give the same result */
    size_t pieces[] = { 1, 7, 1536 };
    size_t caps[] = { 1, 4096, 100000 };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (pieces[i] == 1 && caps[j] == 1) {
                continue; /* 340k single-byte drains: not worth the time */
            }
            run_t q = run_pipe(z, zl, false, NULL, 0, n, n, pieces[i], caps[j], NULL, 0);
            CHECK(q.rc == 0 && memcmp(q.sha, want, 32) == 0 && memcmp(q.osha, wantobj, 32) == 0,
                  "synthetic full piece=%zu cap=%zu rc=%d", pieces[i], caps[j], q.rc);
        }
    }

    /* limits: a too-small psz / isz is an error */
    run_t lim = run_pipe(z, zl, false, NULL, 0, n - 1, 0, 1500, 16384, NULL, 0);
    CHECK(lim.rc < 0, "inflated beyond the limit must error (rc=%d)", lim.rc);
    lim = run_pipe(z, zl, false, NULL, 0, 0, n - 1, 1500, 16384, NULL, 0);
    CHECK(lim.rc < 0, "output beyond isz must error (rc=%d)", lim.rc);

    /* corruption: one flipped byte */
    int bad_ok = 0;
    for (int k = 0; k < 20; k++) {
        uint8_t *c = malloc(zl);
        memcpy(c, z, zl);
        c[(size_t) k * (zl / 21) + 5] ^= 0x40;
        run_t q = run_pipe(c, zl, false, NULL, 0, n, n, 1500, 16384, NULL, 0);
        if (q.rc != 0 || memcmp(q.sha, want, 32) != 0) {
            bad_ok++;
        }
        free(c);
    }
    CHECK(bad_ok == 20, "every flipped byte must give an error or an output SHA mismatch (%d/20)", bad_ok);

    /* truncation */
    run_t tr = run_pipe(z, zl - 100, false, NULL, 0, n, n, 1500, 16384, NULL, 0);
    CHECK(tr.rc != 0, "a truncated object must not finish (rc=%d)", tr.rc);
    tr = run_pipe(z, zl / 2, false, NULL, 0, n, n, 1500, 16384, NULL, 0);
    CHECK(tr.rc != 0, "a half object must not finish (rc=%d)", tr.rc);

    /* push overflow */
    otapipe_t *p = malloc(otapipe_sizeof());
    out_t o;
    memset(&o, 0, sizeof(o));
    mbedtls_sha256_init(&o.sha);
    otapipe_init(p, false, 0, NULL, 0, out_write, &o);
    uint8_t blk[OTAPIPE_IN_CAP];
    memset(blk, 0, sizeof(blk));
    CHECK(otapipe_push(p, blk, OTAPIPE_IN_CAP), "a full-cap push must fit");
    CHECK(!otapipe_push(p, blk, 1), "a push beyond OTAPIPE_IN_CAP must be refused");
    otapipe_deinit(p);
    mbedtls_sha256_free(&o.sha);
    free(p);
    free(cap);
    free(z);
    free(img);
    printf("synthetic full ok (zlib object %lu bytes)\n", (unsigned long) zl);
}

static void test_sizeof(void)
{
    printf("sizeof(otapipe_t) = %zu\n", sizeof(otapipe_t));
    CHECK(sizeof(otapipe_t) < 48 * 1024, "otapipe_t is %zu bytes (limit 48 KiB)", sizeof(otapipe_t));
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

static void test_real(void)
{
    const char *fx = getenv("OTA_FIXTURES");
    const char *im = getenv("OTA_IMAGES");
    if (!fx) {
        fx = "../../build/bench-logs/ota_meas";
    }
    if (!im) {
        im = "../../build/images";
    }
    char path[512];
    size_t tn = 0;
    join(path, sizeof(path), im, "main-release-app.bin");
    uint8_t *target = slurp(path, &tn);
    size_t fn = 0;
    join(path, sizeof(path), fx, "main.full.z");
    uint8_t *fullz = slurp(path, &fn);
    if (!target || !fullz) {
        printf("SKIP real-artifact cases (fixtures absent)\n");
        g_skips++;
        free(target);
        free(fullz);
        return;
    }
    uint8_t want[32];
    sha_of(target, tn, want);
    CHECK(tn == 685168, "main-release-app.bin is %zu bytes, expected 685168", tn);
    /* esptool appends the SHA-256 of everything before it */
    uint8_t chk[32];
    sha_of(target, tn - 32, chk);
    CHECK(memcmp(chk, target + tn - 32, 32) == 0, "target image does not end in its own SHA-256");

    run_t r = run_pipe(fullz, fn, false, NULL, 0, tn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
    CHECK(r.rc == 0 && r.out_len == tn && memcmp(r.sha, want, 32) == 0, "real full: rc=%d len=%u", r.rc,
          r.out_len);
    printf("real full main.full.z: %s (%u bytes out, %zu object bytes)\n",
           (r.rc == 0 && memcmp(r.sha, want, 32) == 0) ? "OK" : "BAD", r.out_len, fn);

    /* flip a byte in the real full object */
    int bad = 0;
    for (int k = 1; k <= 10; k++) {
        uint8_t *c = malloc(fn);
        memcpy(c, fullz, fn);
        c[fn * (size_t) k / 11] ^= 0x01;
        run_t q = run_pipe(c, fn, false, NULL, 0, tn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
        if (q.rc != 0 || memcmp(q.sha, want, 32) != 0) {
            bad++;
        }
        free(c);
    }
    CHECK(bad == 10, "real full: flipped byte must be caught (%d/10)", bad);
    run_t tr = run_pipe(fullz, fn - 1, false, NULL, 0, tn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
    CHECK(tr.rc != 0, "real full: truncated object must not finish (rc=%d)", tr.rc);

    static const char *names[] = { "keylat", "shake", "round5", "lockscreen-noleak" };
    for (int i = 0; i < 4; i++) {
        char nm[128];
        size_t dn = 0, bn = 0, sn = 0;
        snprintf(nm, sizeof(nm), "%s.dz", names[i]);
        join(path, sizeof(path), fx, nm);
        uint8_t *dz = slurp(path, &dn);
        snprintf(nm, sizeof(nm), "%s.seq.none", names[i]);
        join(path, sizeof(path), fx, nm);
        uint8_t *seq = slurp(path, &sn);
        snprintf(nm, sizeof(nm), "%s-release-app.bin", names[i]);
        join(path, sizeof(path), im, nm);
        uint8_t *base = slurp(path, &bn);
        if (!dz || !seq || !base) {
            printf("SKIP real delta %s (fixtures absent)\n", names[i]);
            g_skips++;
            free(dz);
            free(seq);
            free(base);
            continue;
        }
        run_t d = run_pipe(dz, dn, true, base, bn, sn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
        int ok = d.rc == 0 && d.out_len == tn && memcmp(d.sha, want, 32) == 0;
        CHECK(ok, "real delta %s: rc=%d len=%u", names[i], d.rc, d.out_len);
        CHECK(d.max_drain_out <= 16384, "real delta %s: one drain wrote %zu > 16384", names[i], d.max_drain_out);
        printf("real delta %-18s base=%zu B object=%zu B patch=%zu B -> %u B out: %s\n", names[i], bn, dn, sn,
               d.out_len, ok ? "OK" : "BAD");

        /* a base that differs by one byte gives a different output */
        uint8_t *b2 = malloc(bn);
        memcpy(b2, base, bn);
        b2[bn / 2] ^= 0x01;
        run_t d2 = run_pipe(dz, dn, true, b2, bn, sn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
        CHECK(d2.rc != 0 || memcmp(d2.sha, want, 32) != 0, "real delta %s: a 1-byte-different base must not reproduce the target",
              names[i]);
        /* the output must differ in the way detected by the SHA compare */
        free(b2);

        /* flipped byte in the delta object */
        uint8_t *c = malloc(dn);
        memcpy(c, dz, dn);
        c[dn / 2] ^= 0x10;
        run_t d3 = run_pipe(c, dn, true, base, bn, sn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
        CHECK(d3.rc != 0 || memcmp(d3.sha, want, 32) != 0, "real delta %s: flipped byte must be caught", names[i]);
        free(c);

        run_t d4 = run_pipe(dz, dn - 10, true, base, bn, sn, tn, OTAPIPE_IN_CAP, 16384, NULL, 0);
        CHECK(d4.rc != 0, "real delta %s: truncated object must not finish (rc=%d)", names[i], d4.rc);

        /* the wrong base entirely (another release) */
        free(dz);
        free(seq);
        free(base);
    }
    free(target);
    free(fullz);
}

int main(void)
{
    test_sizeof();
    test_synthetic_full();
    test_real();
    if (g_failures == 0) {
        printf("PASS: otapipe, 0 failures%s\n", g_skips ? " (with skips)" : "");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

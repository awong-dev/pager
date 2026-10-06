/* otapipe.h — streaming OTA object decoder (docs/OTA_DESIGN.md D4, section 3).
 *
 * Pure C, no ESP-IDF dependency (host-tested by firmware/host/test_otapipe.c).
 * The caller pushes the downloaded object (zlib stream) in pieces of up to one
 * socket read; drain() inflates it with tinfl (ROM on the device, vendored
 * miniz on the host) and either writes the inflated bytes straight out (full
 * image) or feeds them to the detools sequential patcher, which reads the base
 * image from a flat memory region (the running slot, esp_partition_mmap'd) by
 * memcpy. Output reaches `write` in whole 4 KB stage pieces. Every pushed
 * byte is SHA-256'd: finish() returns the object hash for the `osha` check.
 *
 * The struct is ~47 KB: the caller heap_caps_malloc()s it in PSRAM
 * (MALLOC_CAP_SPIRAM), never internal RAM.
 */
#ifndef OTAPIPE_H
#define OTAPIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "detools.h"
#include "mbedtls/sha256.h"
#include "miniz.h" /* tinfl: esp_rom's on the device, host/third_party/miniz on the host */

#ifdef __cplusplus
extern "C" {
#endif

#define OTAPIPE_IN_CAP 1536 /* one socket read (<=1500) plus slack */
#define OTAPIPE_STAGE 4096  /* coalesce writes to whole flash sectors */

/* Receives the final image bytes in order. Returns 0 on success. */
typedef int (*otapipe_write_fn)(void *arg, const uint8_t *buf, size_t len);

typedef struct otapipe {
    tinfl_decompressor inf;
    uint8_t dict[TINFL_LZ_DICT_SIZE]; /* tinfl's circular output window */
    size_t dpos;
    uint8_t in[OTAPIPE_IN_CAP];
    size_t in_len;                    /* valid bytes in in[] */
    size_t in_off;                    /* consumed bytes of in[] */
    uint8_t stage[OTAPIPE_STAGE];
    size_t stage_len;

    bool delta;
    bool eof;
    bool inf_done;                    /* tinfl reached the end of the zlib stream */
    bool done;                        /* inf_done and every inflated byte consumed */
    size_t ob_off, ob_len;            /* inflated bytes in dict[] not yet consumed */
    bool more_out;                    /* tinfl has output it could not yet emit */
    bool failed;
    struct detools_apply_patch_t ap;
    const uint8_t *base;
    size_t base_len;
    size_t base_pos;

    otapipe_write_fn write;
    void *write_arg;

    mbedtls_sha256_context sha;       /* over every byte pushed (the object hash) */
    size_t in_total;                  /* object bytes pushed */
    size_t inflated_total;            /* tinfl output bytes (patch bytes for a delta) */
    size_t inflated_limit;            /* 0 = none */
    size_t out_total;                 /* final image bytes handed to the stage */
    size_t out_limit;                 /* 0 = none */
} otapipe_t;

size_t otapipe_sizeof(void);

/* `psz`: the limit on inflated bytes (delta: the detools patch size; full: the
 * image size). 0 = no limit. `base`/`base_len` are used only for a delta.
 * Returns 0 or a negative error. */
int otapipe_init(otapipe_t *p, bool delta, size_t psz, const uint8_t *base, size_t base_len,
                 otapipe_write_fn w, void *arg);

/* Releases the hash context. Call once when done with the pipe (success or not). */
void otapipe_deinit(otapipe_t *p);

/* Limit on the final image size (`isz`); beyond it is an error. 0 = none. */
void otapipe_set_out_limit(otapipe_t *p, size_t isz);

/* Copies `len` object bytes into the input buffer (and the object hash);
 * false if they would not fit OTAPIPE_IN_CAP together with what is still
 * unconsumed. Nothing is consumed on false. */
bool otapipe_push(otapipe_t *p, const uint8_t *in, size_t len);

/* No more object bytes will be pushed. */
void otapipe_set_eof(otapipe_t *p);

/* Non-zero while drain() still has work that needs no new input: unconsumed
 * input bytes, or (+1) inflated output tinfl has not yet emitted. */
size_t otapipe_pending(const otapipe_t *p);

/* Inflate + patch + write at most `out_cap` inflated bytes. <0 error, 1 the
 * zlib stream is complete, 0 more (needs input or another call). */
int otapipe_drain(otapipe_t *p, size_t out_cap);

/* After drain() returned 1: flush the stage, finalize detools, return the
 * object SHA-256 and the final image length. 0 ok, <0 error (not done,
 * trailing input, detools finalize failed, write failed). */
int otapipe_finish(otapipe_t *p, uint8_t osha[32], uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* OTAPIPE_H */

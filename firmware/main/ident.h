/* ident.h — device identity, read once at boot from NVS namespace "ident".
 *
 * Authority: docs/DEVICE_PLAN.md §3.4 (what the device keeps), §2.7
 * (device-side storage). The bundle fetched over the bootstrap hop
 * (§3.2) is written here by setup.c (F3.5); net.cpp (F3.4) reads it
 * through the getters below instead of its own constexpr credentials.
 *
 * This module does no NVS init itself: main.c calls nvs_flash_init()
 * before ident_load(). It never touches the modem and has no power
 * effect of its own beyond the flash read.
 *
 * No dynamic allocation: ident_load() populates one static ident_t and
 * every getter returns a pointer into it or a copy of a scalar.
 */
#ifndef IDENT_H
#define IDENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing constants. device_id per docs/PROTOCOL.md §1
 * (`^[a-z0-9][a-z0-9-]{2,23}$`, max len 24). The others have no wire
 * length limit of their own; sized generously against the bootstrap
 * bundle examples in docs/DEVICE_PLAN.md §3.2/§3.4.
 * --------------------------------------------------------------------- */
#define IDENT_DEV_ID_MAX 25   /* 24 chars + NUL, PROTOCOL.md §1 */
#define IDENT_MQTT_PW_MAX 65  /* 64 chars + NUL */
#define IDENT_KDEV_LEN 32     /* K_dev, fixed 32-byte HMAC key, DEVICE_PLAN.md §2.3 */
#define IDENT_HOST_MAX 128    /* broker hostname + NUL */
#define IDENT_CA_MAX 4096     /* PEM root CA, DEVICE_PLAN.md §3.4: "ca <= 4 kB" */
#define IDENT_APN_MAX 32      /* carrier APN + NUL; empty = carrier default */
#define IDENT_LABEL_MAX 32    /* display-only device label, e.g. "Kid 1" */
#define IDENT_CA_HASH_LEN 32  /* SHA-256 of the CA currently written to modem slot 12, §3.3 */

/* ident_t.flags bits (docs/DEVICE_PLAN.md §2.6, §9 item 2/H2). */
#define IDENT_FLAG_REQ_SIG (1u << 0) /* device verifies/signs with K_dev when set */

typedef struct {
    char dev_id[IDENT_DEV_ID_MAX];
    char mqtt_pw[IDENT_MQTT_PW_MAX];
    uint8_t kdev[IDENT_KDEV_LEN];
    char host[IDENT_HOST_MAX];
    uint16_t port;
    char ca[IDENT_CA_MAX];
    size_t ca_len; /* strlen(ca), excludes the NUL */
    char apn[IDENT_APN_MAX]; /* empty string = carrier default */
    uint32_t flags;
    char label[IDENT_LABEL_MAX];
    uint8_t ca_hash[IDENT_CA_HASH_LEN];
    /* docs/DEVICE_PLAN.md §2.5 counter epoch. Widened u16 -> u32 in
     * docs/V02_DESIGN.md §3 (4096 cold boots was a guaranteed exhaustion
     * failure mode); see ident.c's ident_load() for the one-time "n_epoch"
     * (u16) -> "n_epoch32" (u32) NVS migration. */
    uint32_t n_epoch;
    uint8_t claimed;  /* set on the first verified `book`, §4.3 — display only */
} ident_t;

/* Reads namespace "ident" from NVS (opened read-only, closed before
 * returning). Requires nvs_flash_init() to have already succeeded.
 * Returns false, and leaves the getters undefined, if any of `dev_id`,
 * `mqtt_pw`, `kdev`, `host`, `port`, `ca` is missing, oversized, or if
 * `dev_id` fails the docs/PROTOCOL.md §1 regex. `flags`, `label`,
 * `apn`, `ca_hash`, `n_epoch`, `claimed` default to zero/empty when
 * absent. No modem or radio access; NVS reads only. */
bool ident_load(void);

/* Writes every field of `id` to the "ident" namespace and commits, then (on
 * success only) copies `*id` into the in-memory copy the ident_get_*()
 * getters read from. v0.2 bug fix #2 (docs/V02_DESIGN.md §2.2): earlier this
 * only wrote NVS, so e.g. ident_get_n_epoch() kept returning the pre-bump
 * value for the rest of the power session after modes.c's
 * on_auth_epoch_wrap() ran — every subsequent signed publish that boot used
 * a stale (already-persisted-as-superseded) epoch. Caller-validated: does
 * not re-check the dev_id regex. Used by setup.c (F3.5) after a bootstrap
 * fetch and by the rotate/re-home path (§3.5). No modem or radio access. */
bool ident_store(const ident_t *id);

/* Erases the entire "ident" namespace, so the next ident_load() returns
 * false. Used when re-provisioning (§3.5's "Set up again"). No modem or
 * radio access. */
bool ident_erase(void);

/* Getters. Valid only after ident_load() has returned true. */
const char *ident_get_dev_id(void);
const char *ident_get_mqtt_pw(void);
const uint8_t *ident_get_kdev(void); /* IDENT_KDEV_LEN bytes */
const char *ident_get_host(void);
uint16_t ident_get_port(void);
const char *ident_get_ca(void); /* NUL-terminated PEM */
size_t ident_get_ca_len(void);
const char *ident_get_apn(void); /* empty string = carrier default */
uint32_t ident_get_flags(void);
const char *ident_get_label(void);
const uint8_t *ident_get_ca_hash(void); /* IDENT_CA_HASH_LEN bytes */
uint32_t ident_get_n_epoch(void); /* v0.2/§3: widened from uint16_t */
uint8_t ident_get_claimed(void);

/* v0.2 §2.4 ("Empty CA slot" bug fix): whether modem TLS cert slot 12 is
 * known, from a prior boot, to already hold a certificate (a real pinned
 * CA, or net.cpp's ISRG Root X2 placeholder when none is pinned) — lets
 * net_init()/net_tls_profile_bootstrap() skip the NVRAM write once it has
 * happened once in the device's lifetime. Independent of ident_load()'s
 * "required fields" contract: this reads NVS key "slot12" (u8) directly,
 * on its own nvs_open(), so it works even when no full identity exists yet
 * (the bootstrap path, before setup.c's first ident_store()). Returns false
 * (never "unreadable") if the namespace/key does not exist or on any read
 * error — a false negative here just costs one extra (harmless, idempotent)
 * NVRAM write, whereas a false positive would leave slot 12 believed
 * populated when it is not. No modem or radio access. */
bool ident_get_slot12_populated(void);

/* Records that slot 12 now holds a certificate (docs/V02_DESIGN.md §2.4).
 * Same "own nvs_open(), no full identity required" independence as
 * ident_get_slot12_populated() above. No modem or radio access. */
bool ident_set_slot12_populated(void);

#ifdef __cplusplus
}
#endif

#endif /* IDENT_H */

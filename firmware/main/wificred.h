/* wificred.h — WiFi credential store (docs/WIFI_DESIGN.md §4,
 * docs/WIFI_TASKS.md W2).
 *
 * Split the usual way (ident.c/lock.c): everything above the `#ifdef
 * ESP_PLATFORM` banner is pure C, no ESP-IDF dependency, host-tested by
 * firmware/host/test_wificred.c — SSID/PSK validation and an in-memory,
 * replace-wholesale `wificred_set_t` of up to WIFICRED_MAX_NETS (2) entries.
 * Below the banner: NVS namespace `wifi` (deliberately separate from
 * `ident` — design §4: "a credential rotation never risks the identity"),
 * following ident.c's own read/write pattern.
 *
 * Keys (docs/WIFI_DESIGN.md §4): `en` (u8), `n` (u8, 0-2), `s0`/`p0`,
 * `s1`/`p1` (str, SSID/PSK), `ch0`/`ch1` (u8, the cached home channel from
 * the last successful association, 0 = unknown/never learned).
 *
 * HARD RULE (this task's own, non-negotiable): no function in this file
 * formats a PSK into a string, and no ESP_LOG* call in this file takes a
 * PSK as an argument. `wifi status` (main.c, a later task) prints
 * `<set>`/`<unset>`, never a PSK value — this module exposes no formatter
 * that could be misused for that. A reviewer must be able to grep this file
 * for `psk` and see nothing printable reach an ESP_LOG* call.
 *
 * NVS is not encrypted on this device today (docs/WIFI_DESIGN.md §4: "a
 * known limit, same as the broker password already stored in ident") — a
 * cleared/replaced PSK is overwritten (nvs_erase_key on the unused slot),
 * not securely wiped at the physical flash level, same limit ident.c's own
 * string fields already carry.
 */
#ifndef WIFICRED_H
#define WIFICRED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFICRED_MAX_NETS 2
#define WIFICRED_SSID_MAX 33 /* 32 bytes + NUL, docs/WIFI_DESIGN.md §4 */
#define WIFICRED_PSK_MAX 64  /* 63 bytes + NUL */

typedef struct {
    char ssid[WIFICRED_SSID_MAX];
    char psk[WIFICRED_PSK_MAX];
    uint8_t channel; /* cached home channel, 0 = unknown/never learned */
} wificred_net_t;

typedef struct {
    uint8_t count; /* 0..WIFICRED_MAX_NETS */
    wificred_net_t nets[WIFICRED_MAX_NETS];
} wificred_set_t;

/* SSID: 1-32 bytes, no embedded NUL (an embedded NUL cannot round-trip
 * through nvs_set_str()/nvs_get_str(), which this module's ESP half uses —
 * a silent truncation there would be worse than an outright rejection). */
bool wificred_valid_ssid(const char *ssid, size_t len);

/* PSK: 8-63 bytes (WPA2-PSK's own passphrase length, docs/WIFI_DESIGN.md
 * §4), no embedded NUL (same nvs_set_str()/nvs_get_str() round-trip
 * requirement as the SSID check above). */
bool wificred_valid_psk(const char *psk, size_t len);

/* A candidate network as raw bytes + explicit lengths (not yet a
 * NUL-terminated wificred_net_t) — the shape a cfg-submap parser (W3) or the
 * setup console (`wifi set <ssid> <psk>`, W5) hands in before validation. */
typedef struct {
    const char *ssid;
    size_t ssid_len;
    const char *psk;
    size_t psk_len;
} wificred_candidate_t;

/* Validates `candidates[0..count)` as a WHOLE batch (every SSID/PSK boundary,
 * no embedded NUL, `count <= WIFICRED_MAX_NETS`) and, only if the entire
 * batch is valid, replaces `*out` wholesale — `*out` is never partially
 * applied or even touched on a rejected batch, same "reject the whole list
 * if any entry is bad" contract sms_parse_cfg_submap() documents.
 * `count == 0` is a valid, empty batch (clears `*out` to zero networks) —
 * docs/WIFI_DESIGN.md §4: "`nets: []` clears them." Learned channels
 * (wificred_net_t.channel) are NOT part of a candidate (a push never carries
 * one) — every network built by this function starts with channel 0
 * (unknown); wificred_note_channel() is the only way a channel is ever set,
 * and only after a real association. */
bool wificred_apply_candidates(const wificred_candidate_t *candidates, uint8_t count, wificred_set_t *out);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Device wiring: NVS namespace "wifi".
 * --------------------------------------------------------------------- */

/* Loads NVS namespace "wifi" into RAM (both the `en` flag and the network
 * set). Call once from modes_boot(), same slot ident_load()/catrust_init()
 * occupy. A missing namespace (never configured) leaves both `en` false and
 * the set empty — not an error. A partially-written slot (a crash mid-store
 * left `n` ahead of what was actually committed) is never trusted: loading
 * stops at the first incomplete slot rather than guessing, so the effective
 * count silently shrinks to whatever was fully present. No modem or
 * sleep-state effect; NVS reads only. Never logs a PSK value (this file's
 * own hard rule, above). */
void wificred_init(void);

/* Plain reads of the RAM cache wificred_init()/wificred_store()/
 * wificred_set_enabled() keep current — no NVS I/O of their own beyond what
 * wificred_init() already did. */
bool wificred_enabled(void);
uint8_t wificred_count(void);
/* `out` is filled with a COPY (including the actual PSK bytes — this is the
 * one legitimate consumer, wifi_sta.c's own STA config in a later task, not
 * a display/log path) iff `idx < wificred_count()`. */
bool wificred_get(uint8_t idx, wificred_net_t *out);

/* Persists the `en` flag alone (console `wifi on`/`wifi off`, `cfg.wifi.en`
 * without `nets` — design §4: "so the web app can toggle WiFi without
 * re-sending the PSK"). One NVS write, no modem/RRC effect. */
bool wificred_set_enabled(bool enabled);

/* Persists `*set` wholesale: `n` plus `s{i}`/`p{i}`/`ch{i}` for every slot
 * `i < set->count`, and ERASES (nvs_erase_key(), not merely zeroed) the
 * `s{i}`/`p{i}`/`ch{i}` keys for every slot `i >= set->count` — a shrinking
 * set must never leave a stale, unreferenced PSK sitting in flash. Does not
 * touch `en` (a separate concern, wificred_set_enabled() above) — matches
 * `cfg.wifi`'s own independent `en`/`nets` shape (docs/WIFI_DESIGN.md §4).
 * One NVS commit, no modem/RRC effect. Never logs a PSK value. */
bool wificred_store(const wificred_set_t *set);

/* `wifi clear` (console, W5): equivalent to wificred_store() with an empty
 * set (count 0) — clears every stored network, leaves `en` untouched. */
bool wificred_clear(void);

/* Learned after a real association (wifi_sta.c, a later task): persists
 * ONLY the `ch{idx}` key (docs/WIFI_DESIGN.md §4's "cached home channel"),
 * one small NVS write, no full-set rewrite. No-op if `idx >= wificred_count()`. */
void wificred_note_channel(uint8_t idx, uint8_t channel);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* WIFICRED_H */

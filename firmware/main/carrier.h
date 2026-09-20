// carrier.h — the pager-side choice of carrier APN, with presets built into
// the firmware.
//
// Why this lives on the pager (owner decision, 2026-09-20): the APN is needed
// to attach at all, so it cannot arrive over the air, and a blank APN is not
// a safe default. Found on hardware: a US Mobile "Dark Star" SIM (an AT&T
// reseller) attaches fine with a blank APN but gets a crippled data path on
// which every TLS handshake stalls; with APN "ereseller" everything works.
// The SIM cannot be identified reliably (AT&T's own IMSI/ICCID ranges, no
// operator-name file, a GID1 shared with other resellers), so a person picks
// the carrier once, on the pager: the `carrier` console command in Setup
// mode, or Device -> Carrier. It is stored in its own NVS namespace, so it
// survives "Set up again" and a factory reset of the identity.
//
// Precedence when attaching (net.cpp):
//   1. an `;apn=` typed as part of the setup code (explicit, this one time)
//   2. the person's fixed choice on this pager, if they made one
//   3. automatic detection from the SIM (IMSI network code + GID1), the way
//      Android does it. US Mobile documents "MVNO type GID, value 20FF" for
//      Dark Star, and that is exactly what the bench SIM's EF_GID1 holds.
//   4. the APN the setup bundle carried (ident)
//   5. blank: let the network choose
#ifndef CARRIER_H
#define CARRIER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARRIER_APN_MAX 32 /* matches IDENT_APN_MAX: 31 characters + NUL */

typedef struct {
    const char *label; /* shown to the person; keep it short enough for one 296 px row */
    const char *apn;   /* "" = let the network choose */
    /* Automatic detection, the way Android's apns-conf.xml does it: the SIM's
     * home network (first 6 digits of the IMSI; US MNCs are 3 digits) plus,
     * for an MVNO that shares its host's network code, a prefix of EF_GID1 as
     * upper-case hex. NULL/"" plmns = never auto-detected. Several PLMNs are
     * separated by spaces. */
    const char *plmns;
    const char *gid1_prefix; /* "" = any GID1 */
} carrier_preset_t;

/* Preset 0 is always "Automatic" (detect from the SIM, else blank) and preset
 * 1 is always "Carrier default" (force a blank APN). Add a carrier only with a
 * source for its APN and match data, and ideally a pager seen working on it. */
#define CARRIER_PRESET_AUTO 0
#define CARRIER_PRESET_BLANK 1
size_t carrier_preset_count(void);
const carrier_preset_t *carrier_preset_at(size_t i);

/* 3GPP TS 23.003 section 9.1: letters, digits, dots and hyphens; must start and end
 * with a letter or digit; at most CARRIER_APN_MAX - 1 characters. "" is valid. */
bool carrier_apn_valid(const char *apn);

/* Index of the first real carrier preset (>= 2) whose APN equals `apn`, or -1. */
int carrier_preset_index_for(const char *apn);

/* Automatic detection. `imsi` is the decimal IMSI; `gid1_hex` is EF_GID1 as
 * hex (any case) or NULL/"" when the SIM has none. Returns the matching
 * preset, or NULL. Pure: host-tested. */
const carrier_preset_t *carrier_detect(const char *imsi, const char *gid1_hex);

typedef enum {
    CARRIER_MODE_AUTO = 0,  /* nothing chosen: detect from the SIM */
    CARRIER_MODE_FIXED = 1, /* the person chose an APN (possibly blank) */
} carrier_mode_t;

#ifdef ESP_PLATFORM
carrier_mode_t carrier_get_mode(void);
/* The person's fixed choice ("" = blank). Meaningless in CARRIER_MODE_AUTO. */
const char *carrier_get_apn(void);
/* "Automatic", "Carrier default", a preset's label, or "custom". */
const char *carrier_get_label(void);
/* All persist to NVS (namespace "carrier") and take effect at the next
 * attach, i.e. after a restart or on the next `setup`. */
bool carrier_select_preset(size_t i);
bool carrier_set_custom(const char *apn);
/* What the last automatic detection found, for display ("" if nothing). */
void carrier_note_detected(const char *label);
const char *carrier_last_detected(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CARRIER_H */

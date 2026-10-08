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

/* Bearer: TLS from the modem to the broker (direct), or plain MQTT to the
 * carrier's proxy that opens the TLS leg (beam, Soracom Beam). */
typedef enum { CARRIER_BEARER_DIRECT = 0, CARRIER_BEARER_BEAM = 1 } carrier_bearer_t;
/* Values equal the AT+CGAUTH <auth_type> argument (0 none, 1 PAP, 2 CHAP). */
typedef enum { CARRIER_AUTH_NONE = 0, CARRIER_AUTH_PAP = 1, CARRIER_AUTH_CHAP = 2 } carrier_auth_t;
/* Runtime override (NVS carrier/bearer, absent = auto). */
typedef enum { CARRIER_BEARER_OVR_AUTO = 0, CARRIER_BEARER_OVR_DIRECT = 1, CARRIER_BEARER_OVR_BEAM = 2 } carrier_bearer_ovr_t;

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
    /* docs/SORACOM_DESIGN.md 3.1: how MQTT reaches the broker, PDP credentials
     * for the attach, and whether the pager may send SMS through the modem. */
    carrier_bearer_t bearer;
    carrier_auth_t auth_proto;
    const char *auth_user; /* NULL when auth_proto is NONE */
    const char *auth_pass;
    bool sms_mo;
    /* Optional second detection rule: ICCID prefixes (digits, space-separated).
     * A SIM whose ICCID starts with one matches this preset whatever its IMSI.
     * NULL = none. */
    const char *iccid_prefixes;
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
 * preset, or NULL. `iccid` may be NULL/"" (then only the IMSI/GID rule applies). Pure: host-tested. */
const carrier_preset_t *carrier_detect(const char *imsi, const char *gid1_hex, const char *iccid);

/* The preset in force, pure: `fixed` selects the person's fixed choice
 * (`fixed_apn`, "" = Carrier default) instead of detection. Precedence as the
 * header comment: typed APN, fixed choice, detection. A typed APN equal to the
 * detected preset's own APN still returns that preset (typing the preset's APN
 * is not a custom APN); any other typed APN returns NULL, as does Carrier
 * default or a custom fixed APN. NULL means direct bearer, no PDP auth, SMS ok. */
const carrier_preset_t *carrier_resolve(bool fixed, const char *fixed_apn, const char *imsi, const char *gid1_hex,
                                        const char *iccid, const char *typed_apn);

/* The bearer actually used: the preset's (NULL = direct), changed by the
 * override. Forcing beam is refused (direct) unless the preset in force is a
 * Beam preset: beam.soracom.io only means something on Soracom's network and
 * the MQTT password would otherwise go in clear text to whatever host answers. */
carrier_bearer_t carrier_bearer_resolve(const carrier_preset_t *p, carrier_bearer_ovr_t ovr);

const char *carrier_auth_name(carrier_auth_t a);   /* "none" / "PAP" / "CHAP" */
const char *carrier_bearer_ovr_name(carrier_bearer_ovr_t o); /* "auto" / "direct" / "beam" */

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
/* carrier_resolve() with the stored fixed choice (design 3.1's
 * carrier_effective(imsi, gid1, typed_apn)). */
const carrier_preset_t *carrier_effective(const char *imsi, const char *gid1_hex, const char *iccid, const char *typed_apn);
/* NVS carrier/bearer. Read from NVS on each call (cheap); the net layer reads
 * it once per attach and caches the result. set returns false on NVS failure. */
carrier_bearer_ovr_t carrier_bearer_override_get(void);
bool carrier_bearer_override_set(carrier_bearer_ovr_t o);
/* NVS carrier/auth: 1 = this pager has written AT+CGAUTH credentials to the modem
 * (which keeps them across resets), so a later carrier without credentials must clear them. */
bool carrier_auth_flag_get(void);
void carrier_auth_flag_set(bool on);
/* What the last automatic detection found, for display ("" if nothing). */
void carrier_note_detected(const char *label);
const char *carrier_last_detected(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CARRIER_H */

// Host test for carrier.c's pure half: APN validation and the Android-style
// automatic detection (IMSI network code + EF_GID1 prefix).
#include <stdio.h>
#include <string.h>

#include "carrier.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

int main(void)
{
    CHECK(carrier_preset_count() >= 3, "need Automatic, Carrier default and at least one carrier");
    CHECK(strcmp(carrier_preset_at(CARRIER_PRESET_AUTO)->label, "Automatic") == 0, "preset 0");
    CHECK(carrier_preset_at(CARRIER_PRESET_BLANK)->apn[0] == '\0', "preset 1 must be blank");
    CHECK(carrier_preset_at(carrier_preset_count()) == NULL, "out of range must be NULL");

    // The bench SIM: US Mobile Dark Star, network 310410, EF_GID1 = 20FF.
    const carrier_preset_t *p = carrier_detect("310410123456789", "20FF", NULL);
    CHECK(p && strcmp(p->apn, "ereseller") == 0, "310410 + 20FF must detect ereseller");
    p = carrier_detect("310280123456789", "20ff", NULL);
    CHECK(p && strcmp(p->apn, "ereseller") == 0, "310280 + lower-case gid must detect ereseller");
    p = carrier_detect("310410123456789", "20FFFFFFFFFFFFFF", NULL);
    CHECK(p != NULL, "a longer GID1 with the right prefix must match");

    // Near misses must NOT match: a wrong APN is worse than a blank one.
    CHECK(carrier_detect("310410123456789", "DEFF", NULL) == NULL, "another AT&T reseller (Tracfone) must not match");
    CHECK(carrier_detect("310410123456789", "", NULL) == NULL, "an AT&T SIM with no GID1 must not match");
    CHECK(carrier_detect("310410123456789", NULL, NULL) == NULL, "NULL GID1 must not match");
    CHECK(carrier_detect("310410123456789", "2", NULL) == NULL, "a GID1 shorter than the prefix must not match");
    CHECK(carrier_detect("310260123456789", "20FF", NULL) == NULL, "T-Mobile (Google Fi) with that GID must not match");
    CHECK(carrier_detect("31041", "20FF", NULL) == NULL, "a truncated IMSI must not match");
    CHECK(carrier_detect(NULL, "20FF", NULL) == NULL, "NULL IMSI must not match");

    CHECK(carrier_preset_index_for("ereseller") >= 2, "ereseller is a carrier preset");
    CHECK(carrier_preset_index_for("") == -1, "blank is not a carrier preset");
    CHECK(carrier_preset_index_for("something.else") == -1, "unknown APN is custom");

    // Soracom: a 5-digit PLMN entry matches by prefix; Beam bearer, PAP sora/sora, no modem SMS.
    p = carrier_detect("311588112011642", "", NULL);
    CHECK(p && strcmp(p->label, "Soracom") == 0, "Soracom IMSI must detect Soracom");
    CHECK(p && p->bearer == CARRIER_BEARER_BEAM && p->auth_proto == CARRIER_AUTH_PAP, "Soracom is beam/PAP");
    CHECK(p && strcmp(p->auth_user, "sora") == 0 && strcmp(p->auth_pass, "sora") == 0, "Soracom credentials");
    CHECK(p && strcmp(p->apn, "soracom.io") == 0 && !p->sms_mo, "Soracom APN, no SMS MO");
    CHECK(carrier_detect("311599112011642", "", NULL) == NULL, "another 295 network must not match");
    p = carrier_detect("311588112011642", "FFFF", NULL);
    CHECK(p != NULL, "Soracom has no GID1 requirement (observed EF_GID1 = FFFF)");

    // Detection by ICCID (issuer prefix 8942310), independent of the IMSI.
    const char *soracom_iccid = "8942310023000016420";
    p = carrier_detect("999999123456789", "", soracom_iccid);
    CHECK(p && strcmp(p->label, "Soracom") == 0, "Soracom by ICCID only (other IMSI)");
    p = carrier_detect(NULL, NULL, soracom_iccid);
    CHECK(p && strcmp(p->label, "Soracom") == 0, "Soracom by ICCID only (IMSI unknown)");
    p = carrier_detect("311588112011642", "FFFF", NULL);
    CHECK(p && strcmp(p->label, "Soracom") == 0, "Soracom by IMSI only (observed SIM, no ICCID)");
    p = carrier_detect("310410123456789", "20FF", "8901260123456789012");
    CHECK(p && strcmp(p->apn, "ereseller") == 0, "US Mobile IMSI with a non-Soracom ICCID stays US Mobile");
    CHECK(carrier_detect("310410123456789", "", "8942309999999999999") == NULL, "near-miss ICCID prefix");
    p = carrier_resolve(false, "", "999999123456789", "", soracom_iccid, NULL);
    CHECK(p && p->bearer == CARRIER_BEARER_BEAM, "resolve by ICCID");

    // carrier_resolve: typed > fixed > detected.
    const char *soracom_imsi = "311588112011642";
    p = carrier_resolve(false, "", soracom_imsi, "", NULL, NULL);
    CHECK(p && strcmp(p->label, "Soracom") == 0 && p->bearer == CARRIER_BEARER_BEAM, "auto + Soracom IMSI");
    p = carrier_resolve(false, "", "310410123456789", "20FF", NULL, NULL);
    CHECK(p && strcmp(p->apn, "ereseller") == 0 && p->bearer == CARRIER_BEARER_DIRECT &&
              p->auth_proto == CARRIER_AUTH_NONE && p->sms_mo,
          "US Mobile unchanged: direct/none/sms");
    CHECK(carrier_resolve(false, "", "310410123456789", "", NULL, NULL) == NULL, "AT&T without GID: nothing");
    CHECK(carrier_resolve(false, "", soracom_imsi, "", NULL, "my.custom.apn") == NULL, "typed custom APN: no preset");
    p = carrier_resolve(false, "", soracom_imsi, "", NULL, "soracom.io");
    CHECK(p && p->bearer == CARRIER_BEARER_BEAM, "typing the preset's own APN keeps beam/PAP");
    CHECK(carrier_resolve(false, "", "310410123456789", "20FF", NULL, "soracom.io") == NULL,
          "typed soracom.io on another SIM is custom");
    CHECK(carrier_resolve(true, "", soracom_imsi, "", NULL, NULL) == NULL, "Carrier default on a Soracom SIM: direct/none");
    CHECK(carrier_resolve(true, "my.apn", soracom_imsi, "", NULL, NULL) == NULL, "custom fixed APN: direct/none");
    p = carrier_resolve(true, "soracom.io", "310410123456789", "", NULL, NULL);
    CHECK(p && p->bearer == CARRIER_BEARER_BEAM, "fixed Soracom choice wins over detection");
    p = carrier_resolve(true, "ereseller", soracom_imsi, "", NULL, NULL);
    CHECK(p && p->bearer == CARRIER_BEARER_DIRECT, "fixed US Mobile on a Soracom SIM stays direct");
    CHECK(carrier_resolve(false, "", NULL, NULL, NULL, NULL) == NULL, "no SIM identity: nothing");

    // Bearer override.
    const carrier_preset_t *sp = carrier_resolve(false, "", soracom_imsi, "", NULL, NULL);
    const carrier_preset_t *um = carrier_resolve(false, "", "310410123456789", "20FF", NULL, NULL);
    CHECK(carrier_bearer_resolve(sp, CARRIER_BEARER_OVR_AUTO) == CARRIER_BEARER_BEAM, "auto = beam on Soracom");
    CHECK(carrier_bearer_resolve(sp, CARRIER_BEARER_OVR_DIRECT) == CARRIER_BEARER_DIRECT, "override direct wins");
    CHECK(carrier_bearer_resolve(sp, CARRIER_BEARER_OVR_BEAM) == CARRIER_BEARER_BEAM, "override beam on Soracom");
    CHECK(carrier_bearer_resolve(um, CARRIER_BEARER_OVR_BEAM) == CARRIER_BEARER_DIRECT,
          "forced beam on a non-Beam carrier is refused");
    CHECK(carrier_bearer_resolve(NULL, CARRIER_BEARER_OVR_BEAM) == CARRIER_BEARER_DIRECT, "no preset: beam refused");
    CHECK(carrier_bearer_resolve(NULL, CARRIER_BEARER_OVR_AUTO) == CARRIER_BEARER_DIRECT, "no preset: direct");
    CHECK(strcmp(carrier_auth_name(CARRIER_AUTH_CHAP), "CHAP") == 0, "auth name");

    CHECK(carrier_apn_valid(""), "blank is valid");
    CHECK(carrier_apn_valid("ereseller"), "ereseller");
    CHECK(carrier_apn_valid("m2m.com.attz"), "dotted");
    CHECK(carrier_apn_valid("fast.t-mobile.com"), "hyphen");
    CHECK(!carrier_apn_valid("has space"), "space");
    CHECK(!carrier_apn_valid("semi;colon"), "semicolon");
    CHECK(!carrier_apn_valid("-lead"), "leading hyphen");
    CHECK(!carrier_apn_valid("trail."), "trailing dot");
    CHECK(!carrier_apn_valid("a..b"), "empty label");
    CHECK(!carrier_apn_valid("0123456789012345678901234567890x"), "32 characters is too long");
    CHECK(carrier_apn_valid("0123456789012345678901234567890"), "31 characters fits");
    CHECK(!carrier_apn_valid(NULL), "NULL");

    if (g_failures == 0) {
        printf("PASS: carrier, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

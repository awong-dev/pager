/* placeholder_ca.h — ISRG Root X2, embedded as a placeholder for modem TLS
 * cert slot 12 when no real CA is pinned (docs/V02_DESIGN.md §2.4, "Empty CA
 * slot" bug fix).
 *
 * GOTCHAS.md found, on real hardware, that every TLS profile used for
 * MQTT must name a cert slot even with validation off, or the modem's
 * AT+SQNSMQTT* engine silently sends a plaintext CONNECT instead of TLS.
 * What was never tested is what happens when that *named* slot is empty
 * (UNVERIFIED) -- a real possibility on a factory-fresh modem, or any
 * device whose identity has never pinned a CA (docs/DEVICE_PLAN.md §3.3:
 * "The CA is optional"). This header makes that question not matter: net.cpp
 * writes this cert into slot 12 whenever nothing else has been written
 * there, so the slot is never actually empty at TLS time. It is NEVER
 * validated against -- net.cpp only writes/uses this in the branch where
 * the TLS profile is configured with WALTER_MODEM_TLS_VALIDATION_NONE.
 *
 * ISRG Root X2 was chosen because it is public, EC (small, 790 bytes as a
 * PEM -- comfortably clear of the modem's cert-slot limits), and long-lived
 * (valid until 2040-09-17), so it never needs rotating just to keep serving
 * as a placeholder.
 *
 * Fetched from the local macOS system root store and verified against the
 * publicly documented cert:
 *
 *   security find-certificate -c "ISRG Root X2" -p \
 *     /System/Library/Keychains/SystemRootCertificates.keychain \
 *     > isrg_root_x2.pem
 *   openssl x509 -in isrg_root_x2.pem -noout -subject -fingerprint -sha256
 *
 *     subject=C=US, O=Internet Security Research Group, CN=ISRG Root X2
 *     SHA256 Fingerprint=69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:
 *                         64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70
 *
 * (fingerprint reformatted to fit this comment; openssl prints it as one
 * colon-separated line). This matches the publicly published ISRG Root X2
 * fingerprint. 790 bytes, matching docs/V02_DESIGN.md §2.4's size note.
 */
#ifndef PAGER_PLACEHOLDER_CA_H
#define PAGER_PLACEHOLDER_CA_H

static const char PAGER_PLACEHOLDER_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

#endif /* PAGER_PLACEHOLDER_CA_H */

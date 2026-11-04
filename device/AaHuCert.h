// AaHuCert — the head-unit X.509 certificate + RSA private key the Android Auto
// TLS handshake presents to the phone (the HU is the SSL client; AA uses mutual
// auth, so the phone requests this cert). Embedded as raw DER (cert) + PEM (key)
// so the bare-metal image needs no filesystem to load them.
//
// A real phone only completes the AA handshake against the Google-registered
// aasdk / JVC-Kenwood head-unit certificate (issuer "Google Automotive Link"); a
// self-signed cert is rejected. carsh does NOT ship that cert + key: they live in
// the aasdk project (GPLv3) and are fetched at build time into the git-ignored
// AaHuCert.gen.h by:
//
//     scripts/fetch_aa_cert.sh
//
// The generated header defines kHuCertDer / kHuCertDerLen / kHuKeyPem /
// kHuKeyPemLen (key PEM is NUL-terminated; mbedtls_pk_parse_key wants the
// terminator counted in the length).
//
// The cert uses a non-standard UTCTime ("-0700" timezone offset) that mbedTLS's
// strict X.509 parser rejects, so AaCryptoMbedTls::loadOwnCert bypasses the parser
// and hands mbedTLS the DER verbatim (raw.p/len + version=3). The handshake only
// *transmits* the cert (both ends VERIFY_NONE/OPTIONAL), so dates are never checked.
#ifndef _carsh_aa_hu_cert_h
#define _carsh_aa_hu_cert_h

#if __has_include("AaHuCert.gen.h")
#include "AaHuCert.gen.h"
#else
#error "device/AaHuCert.gen.h missing: run scripts/fetch_aa_cert.sh (fetches the Android Auto head-unit cert from aasdk)"
#endif

#endif  // _carsh_aa_hu_cert_h

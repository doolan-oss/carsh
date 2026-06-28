// AaHuCert — the head-unit X.509 certificate + RSA private key the Android Auto
// TLS handshake presents to the phone (the HU is the SSL client; AA uses mutual
// auth, so the phone requests this cert). Embedded as raw DER (cert) + PEM (key)
// so the bare-metal image needs no filesystem to load them.
//
// This is the REAL aasdk / JVC-Kenwood head-unit certificate registered with
// Google's Android Auto program (issuer "Google Automotive Link"), ported verbatim
// from dylant9/head_unit:main/aa_crypto.c (aa_cert_der_fallback[] +
// aa_private_key_pem_fallback). A real phone only completes the AA handshake against
// this specific cert; a self-signed cert is rejected. PROVENANCE: this cert+key
// originate from the aasdk project and are GPLv3 — note the licensing before ship.
//
// The cert uses a non-standard UTCTime ("-0700" timezone offset) that mbedTLS's
// strict X.509 parser rejects, so AaCryptoMbedTls::loadOwnCert bypasses the parser
// and hands mbedTLS the DER verbatim (raw.p/len + version=3). The handshake only
// *transmits* the cert (both ends VERIFY_NONE/OPTIONAL), so dates are never checked.
#ifndef _carsh_aa_hu_cert_h
#define _carsh_aa_hu_cert_h

#include <cstddef>
#include <cstdint>

namespace carsh::device {

// DER-encoded X.509 head-unit certificate (the registered aasdk cert).
inline constexpr unsigned char kHuCertDer[] = {
    /* [REDACTED: Android Auto head-unit cert, now fetched by scripts/fetch_aa_cert.sh] */ };
inline constexpr std::size_t kHuCertDerLen = sizeof(kHuCertDer);

// PEM-encoded RSA private key matching the certificate above (NUL-terminated;
// mbedtls_pk_parse_key wants the terminator counted in the length).
inline constexpr char kHuKeyPem[] =
    "[REDACTED: Android Auto head-unit key, now fetched by scripts/fetch_aa_cert.sh]\n";
inline constexpr std::size_t kHuKeyPemLen = sizeof(kHuKeyPem);  // includes NUL

}  // namespace carsh::device

#endif  // _carsh_aa_hu_cert_h

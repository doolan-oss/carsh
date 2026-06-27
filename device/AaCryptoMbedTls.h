// AaCryptoMbedTls — the mbedTLS-backed IAaCrypto for the Android Auto TLS channel
// (AA_PLAN rung 4b). The head unit is the SSL *client*: TLS 1.2,
// ECDHE-RSA-AES128-GCM-SHA256, authmode VERIFY_NONE (AA is mutually authenticated
// but against a fixed cert chain we do not validate), driving an in-memory BIO so
// the handshake + record traffic ride the AA SSL_HANDSHAKE / encrypted control
// frames instead of a socket. Ported from dylant9/head_unit:main/aa_crypto.c.
//
// This unit depends only on mbedTLS (not Circle): the entropy source is injected
// (on device, a function backed by Circle's CBcmRandomNumberGenerator; in host
// tests, std::random_device). It therefore compiles into BOTH the device image
// (circle-stdlib's bundled mbedTLS 2.28) and the host test build (system mbedTLS
// 2.28), so the handshake/record plumbing is genuinely host-verified against the
// same library version. What remains [hw]: the LIVE handshake against a real phone
// (needs the Google-registered head-unit cert — see AaHuCert.h) and confirming the
// circle-mbedtls config enables the cipher suite above.
#ifndef _carsh_aa_crypto_mbedtls_h
#define _carsh_aa_crypto_mbedtls_h

#include <cstddef>
#include <cstdint>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "aa/AaMemBio.h"
#include "aa/IAaCrypto.h"

namespace carsh::device {

class AaCryptoMbedTls : public carsh::aa::IAaCrypto {
public:
    // entropy: an mbedTLS-style entropy callback (fills buf with len random bytes,
    // returns 0 on success) used to seed the CTR-DRBG; ctx is passed back to it. On
    // device ALWAYS supply the hardware RNG — bare metal has no entropy pool.
    using EntropyFn = int (*)(void* ctx, unsigned char* buf, size_t len);

    AaCryptoMbedTls(EntropyFn entropy, void* entropyCtx);
    ~AaCryptoMbedTls() override;

    AaCryptoMbedTls(const AaCryptoMbedTls&) = delete;
    AaCryptoMbedTls& operator=(const AaCryptoMbedTls&) = delete;

    // One-time setup: seed the DRBG, load the cert+key, configure + set up the SSL
    // context. Returns false (leaving the object unusable) on any mbedTLS error.
    // init() uses the embedded head-unit cert (AaHuCert.h); initWith() overrides it.
    bool init();
    bool initWith(const unsigned char* certDer, size_t certLen, const char* keyPem,
                  size_t keyLen);

    // --- host-test hooks (no effect on the device's client role) -----------------
    // Run as the TLS server, so an in-memory peer can complete a real handshake.
    void setServer(bool server) { isServer_ = server; }
    // Force the raw-cert bypass (the path the real aasdk cert needs because its
    // non-standard UTCTime makes mbedTLS's x509 parse reject it) even for a cert
    // that would otherwise parse, so that exact code path is host-exercised.
    void setForceRawCert(bool v) { forceRawCert_ = v; }

    int lastError() const { return lastError_; }

    // IAaCrypto.
    std::vector<uint8_t> handshakeStep(const uint8_t* in, size_t len) override;
    bool handshakeDone() const override { return handshakeDone_; }
    std::vector<uint8_t> encrypt(const uint8_t* plain, size_t len) override;
    std::vector<uint8_t> decrypt(const uint8_t* cipher, size_t len) override;

private:
    bool loadOwnCert(const unsigned char* certDer, size_t certLen, const char* keyPem,
                     size_t keyLen);
    static int bioSend(void* ctx, const unsigned char* buf, size_t len);
    static int bioRecv(void* ctx, unsigned char* buf, size_t len);

    EntropyFn entropy_;
    void* entropyCtx_;
    bool isServer_ = false;
    bool forceRawCert_ = false;
    bool inited_ = false;
    bool handshakeDone_ = false;
    int lastError_ = 0;

    carsh::aa::AaMemBio bio_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_ctr_drbg_context drbg_;
    mbedtls_x509_crt cert_;
    mbedtls_pk_context pk_;
};

}  // namespace carsh::device

#endif  // _carsh_aa_crypto_mbedtls_h

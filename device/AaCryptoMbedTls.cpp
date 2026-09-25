// AaCryptoMbedTls.cpp — see AaCryptoMbedTls.h. Faithful port of the reference
// mbedTLS head-unit crypto onto the IAaCrypto seam.
#include "AaCryptoMbedTls.h"

#include <mbedtls/error.h>
#include <mbedtls/ssl_ciphersuites.h>

#include <cstring>

#include "AaHuCert.h"

namespace carsh::device {
namespace {

// The control message id space lives inside the ciphertext, so the largest record
// we ever decrypt in one go is a service-discovery / channel-setup protobuf — a few
// KB. 16 KB matches a TLS record max and the reference's BIO sizing.
constexpr size_t kReadChunk = 16 * 1024;

// Pin the head unit to exactly the suite the DHU capture negotiates
// (ECDHE-RSA-AES128-GCM-SHA256). Restricting the list keeps the ClientHello small
// and deterministic and avoids offering suites the circle-mbedtls config may omit.
const int kCiphersuites[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    0,
};

}  // namespace

// AaMemBio's want-read sentinel must equal mbedTLS's, since it is returned straight
// out of the BIO recv callback into mbedTLS.
static_assert(carsh::aa::AaMemBio::kWantRead == MBEDTLS_ERR_SSL_WANT_READ,
              "AaMemBio::kWantRead must match MBEDTLS_ERR_SSL_WANT_READ");

AaCryptoMbedTls::AaCryptoMbedTls(EntropyFn entropy, void* entropyCtx)
    : entropy_(entropy), entropyCtx_(entropyCtx) {
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_ctr_drbg_init(&drbg_);
    mbedtls_x509_crt_init(&cert_);
    mbedtls_pk_init(&pk_);
}

AaCryptoMbedTls::~AaCryptoMbedTls() {
    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&conf_);
    mbedtls_ctr_drbg_free(&drbg_);
    mbedtls_x509_crt_free(&cert_);
    mbedtls_pk_free(&pk_);
}

int AaCryptoMbedTls::bioSend(void* ctx, const unsigned char* buf, size_t len) {
    return static_cast<AaCryptoMbedTls*>(ctx)->bio_.send(buf, len);
}
int AaCryptoMbedTls::bioRecv(void* ctx, unsigned char* buf, size_t len) {
    return static_cast<AaCryptoMbedTls*>(ctx)->bio_.recv(buf, len);
}

bool AaCryptoMbedTls::loadOwnCert(const unsigned char* certDer, size_t certLen,
                                  const char* keyPem, size_t keyLen) {
    // The private key parses normally (standard PEM). mbedtls_pk_parse_key wants the
    // terminating NUL counted in keyLen.
    int ret = mbedtls_pk_parse_key(&pk_, reinterpret_cast<const unsigned char*>(keyPem),
                                   keyLen, nullptr, 0);
    if (ret != 0) {
        lastError_ = ret;
        return false;
    }

    // The cert: try a normal DER parse first. The real aasdk head-unit cert uses a
    // non-standard UTCTime (a "-0700" timezone offset) that mbedTLS's x509 parser
    // rejects, so on failure (or when forced for host coverage) fall back to handing
    // mbedTLS the DER verbatim. The cert is only *transmitted* during the handshake
    // (both ends are VERIFY_NONE / VERIFY_OPTIONAL), so raw.p + raw.len + version is
    // all that is needed — we never validate dates on it.
    if (forceRawCert_ || mbedtls_x509_crt_parse_der(&cert_, certDer, certLen) != 0) {
        mbedtls_x509_crt_free(&cert_);
        mbedtls_x509_crt_init(&cert_);
        // own_buffer = 0: raw aliases the caller's (static, immortal) DER, so
        // mbedtls_x509_crt_free must NOT try to free it.
        cert_.raw.p = const_cast<unsigned char*>(certDer);
        cert_.raw.len = certLen;
        cert_.version = 3;
        cert_.own_buffer = 0;
    }

    ret = mbedtls_ssl_conf_own_cert(&conf_, &cert_, &pk_);
    if (ret != 0) {
        lastError_ = ret;
        return false;
    }
    return true;
}

bool AaCryptoMbedTls::init() {
    return initWith(kHuCertDer, kHuCertDerLen, kHuKeyPem, kHuKeyPemLen);
}

bool AaCryptoMbedTls::initWith(const unsigned char* certDer, size_t certLen,
                               const char* keyPem, size_t keyLen) {
    if (inited_) return true;

    static const char kPers[] = "carsh-aa-tls";
    int ret = mbedtls_ctr_drbg_seed(&drbg_, entropy_, entropyCtx_,
                                    reinterpret_cast<const unsigned char*>(kPers),
                                    sizeof(kPers) - 1);
    if (ret != 0) {
        lastError_ = ret;
        return false;
    }

    ret = mbedtls_ssl_config_defaults(
        &conf_, isServer_ ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        lastError_ = ret;
        return false;
    }

    // AA's cert chain is fixed and self-signed-style — never CA-validated. As a
    // server (host test only) request the client cert with VERIFY_OPTIONAL so the
    // head unit's mutual-auth cert transmission is exercised end to end.
    mbedtls_ssl_conf_authmode(
        &conf_, isServer_ ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
    mbedtls_ssl_conf_ciphersuites(&conf_, kCiphersuites);
    // Pin TLS 1.2 (the DHU/phone speak exactly this).
    mbedtls_ssl_conf_min_version(&conf_, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&conf_, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);

    if (!loadOwnCert(certDer, certLen, keyPem, keyLen)) return false;

    ret = mbedtls_ssl_setup(&ssl_, &conf_);
    if (ret != 0) {
        lastError_ = ret;
        return false;
    }
    mbedtls_ssl_set_bio(&ssl_, this, bioSend, bioRecv, nullptr);

    inited_ = true;
    return true;
}

std::vector<uint8_t> AaCryptoMbedTls::handshakeStep(const uint8_t* in, size_t len) {
    if (!inited_ || failed_) return {};
    if (in != nullptr && len > 0) bio_.pushIn(in, len);

    if (!handshakeDone_) {
        int ret = mbedtls_ssl_handshake(&ssl_);
        if (ret == 0) {
            handshakeDone_ = true;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            lastError_ = ret;  // fatal handshake failure — the context is now unusable
            failed_ = true;
        }
    }
    // Whatever mbedTLS queued this step (ClientHello, key exchange + Finished, …) is
    // returned to be shipped as one SSL_HANDSHAKE control message.
    return bio_.drainOut();
}

std::vector<uint8_t> AaCryptoMbedTls::encrypt(const uint8_t* plain, size_t len) {
    if (!handshakeDone_ || failed_) return {};
    size_t off = 0;
    while (off < len) {
        int ret = mbedtls_ssl_write(&ssl_, plain + off, len - off);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;  // BIO never blocks; retry to make progress
            }
            lastError_ = ret;  // fatal: mbedTLS marks the context unusable
            failed_ = true;
            return {};
        }
        off += static_cast<size_t>(ret);
    }
    return bio_.drainOut();
}

std::vector<uint8_t> AaCryptoMbedTls::decrypt(const uint8_t* cipher, size_t len) {
    if (!handshakeDone_ || failed_) return {};
    bio_.pushIn(cipher, len);

    std::vector<uint8_t> out;
    uint8_t buf[kReadChunk];
    for (;;) {
        int ret = mbedtls_ssl_read(&ssl_, buf, sizeof(buf));
        if (ret > 0) {
            out.insert(out.end(), buf, buf + ret);
            continue;  // drain any further buffered records
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == 0) break;  // no more input
        // Anything else is fatal for the session (bad MAC after a lost/corrupted
        // record, a fatal alert, the peer's close_notify): mbedTLS leaves the context
        // unusable. Report it — without this a single lost record made every later
        // decrypt return empty while the connection carried on "Running" forever.
        lastError_ = ret;
        failed_ = true;
        return {};
    }
    return out;
}

}  // namespace carsh::device

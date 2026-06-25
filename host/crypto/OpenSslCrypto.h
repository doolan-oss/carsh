// OpenSSL-backed ICrypto for the build server (PLAN.md working method). Provides
// real SHA-256 + Ed25519 verify so OTA verification is genuinely exercised, plus
// host-only test helpers (keygen + sign) to build signed package fixtures.
// The device uses an mbed TLS implementation of the same ICrypto interface.
#pragma once

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "ota/Crypto.h"

namespace carsh::host {

class OpenSslCrypto : public carsh::ota::ICrypto {
public:
    carsh::ota::Sha256Digest sha256(const uint8_t* data,
                                    std::size_t len) override {
        carsh::ota::Sha256Digest out{};
        SHA256(data, len, out.data());
        return out;
    }

    bool ed25519Verify(const carsh::ota::Ed25519PublicKey& pub,
                       const uint8_t* msg, std::size_t msgLen,
                       const carsh::ota::Ed25519Signature& sig) override {
        EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                    pub.data(), pub.size());
        if (!key) return false;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        bool ok = false;
        if (ctx &&
            EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
            ok = EVP_DigestVerify(ctx, sig.data(), sig.size(), msg, msgLen) == 1;
        }
        if (ctx) EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return ok;
    }

    // --- Test-only helpers (not part of ICrypto / device build) -------------

    static bool genKeypair(carsh::ota::Ed25519PublicKey& pub,
                           std::array<uint8_t, 32>& priv) {
        EVP_PKEY* key = nullptr;
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        bool ok = pctx && EVP_PKEY_keygen_init(pctx) == 1 &&
                  EVP_PKEY_keygen(pctx, &key) == 1;
        if (ok) {
            std::size_t pl = pub.size(), sl = priv.size();
            ok = EVP_PKEY_get_raw_public_key(key, pub.data(), &pl) == 1 &&
                 EVP_PKEY_get_raw_private_key(key, priv.data(), &sl) == 1;
        }
        if (key) EVP_PKEY_free(key);
        if (pctx) EVP_PKEY_CTX_free(pctx);
        return ok;
    }

    static bool sign(const std::array<uint8_t, 32>& priv, const uint8_t* msg,
                     std::size_t msgLen, carsh::ota::Ed25519Signature& sig) {
        EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                     priv.data(), priv.size());
        if (!key) return false;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        std::size_t sl = sig.size();
        bool ok = ctx &&
                  EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
                  EVP_DigestSign(ctx, sig.data(), &sl, msg, msgLen) == 1;
        if (ctx) EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return ok;
    }
};

}  // namespace carsh::host

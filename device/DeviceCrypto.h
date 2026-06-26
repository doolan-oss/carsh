// DeviceCrypto — device backend for ota::ICrypto (PLAN.md §2.12).
//
// SHA-256 (image hash) via a vendored public-domain implementation, and Ed25519
// signature verify via TweetNaCl. Note: mbedTLS 2.28 (bundled by circle-stdlib)
// does NOT implement Ed25519 — only PSA algorithm ids exist — so the plan's
// "Ed25519 via mbed TLS" does not hold; TweetNaCl provides it self-contained.
// (See OPEN_QUESTIONS.) Verify-only; no signing/keygen on the device.
#ifndef _carsh_device_crypto_h
#define _carsh_device_crypto_h

#include "ota/Crypto.h"

namespace carsh::device {

class DeviceCrypto : public carsh::ota::ICrypto {
public:
    carsh::ota::Sha256Digest sha256(const uint8_t* data,
                                    std::size_t len) override;
    bool ed25519Verify(const carsh::ota::Ed25519PublicKey& pub,
                       const uint8_t* msg, std::size_t msgLen,
                       const carsh::ota::Ed25519Signature& sig) override;
};

}  // namespace carsh::device

#endif

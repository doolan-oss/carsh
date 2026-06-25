// Crypto abstraction for OTA verification (PLAN.md §2.12, §6).
//
// On the Pi this is backed by mbed TLS; on the build server by OpenSSL. The OTA
// logic only needs SHA-256 (image hashing) and Ed25519 verify (manifest
// signature) — both behind this interface so the package/state-machine logic is
// testable with real signatures on the host.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace carsh::ota {

using Sha256Digest = std::array<uint8_t, 32>;
using Ed25519PublicKey = std::array<uint8_t, 32>;
using Ed25519Signature = std::array<uint8_t, 64>;

class ICrypto {
public:
    virtual ~ICrypto() = default;
    virtual Sha256Digest sha256(const uint8_t* data, std::size_t len) = 0;
    virtual bool ed25519Verify(const Ed25519PublicKey& pub, const uint8_t* msg,
                               std::size_t msgLen,
                               const Ed25519Signature& sig) = 0;
};

}  // namespace carsh::ota

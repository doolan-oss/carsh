// IAaCrypto — the TLS seam for the Android Auto encrypted channels. AA wraps all
// post-handshake channel payloads in TLS records; the handshake itself is
// exchanged as plain SSL_HANDSHAKE control messages. Keeping this behind an
// interface lets the connection state machine (AaConnection) be host-tested with
// a trivial pass-through crypto, while the device uses a real mbedTLS-backed
// implementation (circle-stdlib bundles mbedTLS). See AA_PLAN.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace carsh::aa {

class IAaCrypto {
public:
    virtual ~IAaCrypto() = default;

    // Advance the TLS handshake with handshake bytes received from the phone
    // (empty `in` kicks off the initial ClientHello-equivalent). Returns the
    // handshake bytes to send back as an SSL_HANDSHAKE control message (empty if
    // there is nothing to send this step).
    virtual std::vector<uint8_t> handshakeStep(const uint8_t* in, size_t len) = 0;

    // True once the handshake has completed and encrypt/decrypt are usable.
    virtual bool handshakeDone() const = 0;

    // Wrap plaintext in a TLS record (returns ciphertext) / unwrap a record
    // (returns plaintext). Empty result signals failure.
    virtual std::vector<uint8_t> encrypt(const uint8_t* plain, size_t len) = 0;
    virtual std::vector<uint8_t> decrypt(const uint8_t* cipher, size_t len) = 0;
};

}  // namespace carsh::aa

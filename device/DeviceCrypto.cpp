#include "DeviceCrypto.h"

#include <cstring>
#include <vector>

extern "C" {
#include "sha256/sha256.h"
#include "tweetnacl/tweetnacl.h"

// TweetNaCl references randombytes() (used only by keygen/sign, which we never
// call on the device — verify is deterministic). Provide a stub so the verify-
// only build links. If it were ever called it would (deliberately) produce
// all-zero output; signing on the device is out of scope.
void randombytes(unsigned char* p, unsigned long long n) {
    std::memset(p, 0, std::size_t(n));
}
}

namespace carsh::device {

carsh::ota::Sha256Digest DeviceCrypto::sha256(const uint8_t* data,
                                              std::size_t len) {
    carsh::ota::Sha256Digest out{};
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out.data());
    return out;
}

bool DeviceCrypto::ed25519Verify(const carsh::ota::Ed25519PublicKey& pub,
                                 const uint8_t* msg, std::size_t msgLen,
                                 const carsh::ota::Ed25519Signature& sig) {
    // TweetNaCl verifies an attached signature (sig || msg). Reconstruct it.
    std::vector<unsigned char> sm;
    sm.reserve(64 + msgLen);
    sm.insert(sm.end(), sig.begin(), sig.end());
    sm.insert(sm.end(), msg, msg + msgLen);
    std::vector<unsigned char> m(sm.size());
    unsigned long long mlen = 0;
    return crypto_sign_ed25519_tweet_open(m.data(), &mlen, sm.data(),
                                          (unsigned long long)sm.size(),
                                          pub.data()) == 0;
}

}  // namespace carsh::device

// OTA package (.carup) container + verification (PLAN.md §8.5).
//
// Binary layout (simpler than tar, trivially streamable on bare metal):
//   ["CARUP1"][u8 targetSlotHint][u32 manifestLen][manifest bytes]
//   [64-byte Ed25519 sig over the manifest bytes][u32 imageLen][image bytes]
//
// Verification: the signature authenticates the manifest; the manifest pins the
// image SHA-256. So a valid signature + matching image hash is sufficient — no
// separate image signature is needed.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ota/Crypto.h"
#include "ota/Manifest.h"

namespace carsh::ota {

struct Package {
    Manifest manifest;
    const uint8_t* manifestBytes = nullptr;
    std::size_t manifestLen = 0;
    Ed25519Signature sig{};
    const uint8_t* image = nullptr;
    std::size_t imageLen = 0;
    uint8_t targetSlotHint = 0xFF;  // 0xFF = either
    bool parsed = false;
};

inline constexpr char kPkgMagic[6] = {'C', 'A', 'R', 'U', 'P', '1'};

// Parse the container (no crypto yet). Returns false on structural errors.
// The returned Package borrows into `blob`, which must outlive it.
inline bool parsePackage(const std::vector<uint8_t>& blob, Package& out) {
    auto rdU32 = [&](std::size_t p) {
        return uint32_t(blob[p]) | (uint32_t(blob[p + 1]) << 8) |
               (uint32_t(blob[p + 2]) << 16) | (uint32_t(blob[p + 3]) << 24);
    };
    std::size_t p = 0;
    if (blob.size() < 6 + 1 + 4 || std::memcmp(blob.data(), kPkgMagic, 6) != 0)
        return false;
    p = 6;
    out.targetSlotHint = blob[p++];
    if (p + 4 > blob.size()) return false;
    uint32_t mlen = rdU32(p); p += 4;
    if (p + mlen + 64 + 4 > blob.size()) return false;
    out.manifestBytes = blob.data() + p;
    out.manifestLen = mlen;
    out.manifest = parseManifest(out.manifestBytes, mlen);
    p += mlen;
    std::memcpy(out.sig.data(), blob.data() + p, 64); p += 64;
    uint32_t ilen = rdU32(p); p += 4;
    if (p + ilen > blob.size()) return false;
    out.image = blob.data() + p;
    out.imageLen = ilen;
    out.parsed = true;
    return out.manifest.valid;
}

enum class VerifyResult {
    Ok,
    BadStructure,
    BadSignature,
    ImageHashMismatch,
};

inline VerifyResult verifyPackage(const Package& pkg, ICrypto& crypto,
                                  const Ed25519PublicKey& pub) {
    if (!pkg.parsed || !pkg.manifest.valid) return VerifyResult::BadStructure;
    if (!crypto.ed25519Verify(pub, pkg.manifestBytes, pkg.manifestLen, pkg.sig))
        return VerifyResult::BadSignature;
    Sha256Digest h = crypto.sha256(pkg.image, pkg.imageLen);
    if (h != pkg.manifest.imageSha) return VerifyResult::ImageHashMismatch;
    return VerifyResult::Ok;
}

// Build a package (host/test + the offline signing tool would do the same).
// `signFn(msg,len,sig)` produces the Ed25519 signature over the manifest.
template <typename SignFn>
inline std::vector<uint8_t> buildPackage(const std::string& manifestJson,
                                         const std::vector<uint8_t>& image,
                                         uint8_t targetSlotHint, SignFn signFn) {
    std::vector<uint8_t> v(kPkgMagic, kPkgMagic + 6);
    v.push_back(targetSlotHint);
    auto put32 = [&](uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
    };
    put32(uint32_t(manifestJson.size()));
    v.insert(v.end(), manifestJson.begin(), manifestJson.end());
    Ed25519Signature sig{};
    signFn(reinterpret_cast<const uint8_t*>(manifestJson.data()),
           manifestJson.size(), sig);
    v.insert(v.end(), sig.begin(), sig.end());
    put32(uint32_t(image.size()));
    v.insert(v.end(), image.begin(), image.end());
    return v;
}

}  // namespace carsh::ota

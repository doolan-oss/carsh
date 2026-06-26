// Validates the REAL device OTA crypto backend (device/DeviceCrypto.cpp:
// TweetNaCl Ed25519 + public-domain SHA-256) against OpenSSL. This is how we gain
// confidence in the on-device crypto path without hardware: a package signed
// offline (OpenSSL here, stand-in for the signing tool) must verify with the
// exact backend the Pi runs.
#include <array>
#include <string>
#include <vector>

#include "Test.h"

#ifdef CARSH_HAVE_OPENSSL
#include "DeviceCrypto.h"  // the actual device backend, compiled into the host build
#include "crypto/OpenSslCrypto.h"
#include "ota/Package.h"

using namespace carsh;

TEST(DeviceCrypto, Sha256MatchesKnownVectorAndOpenSSL) {
    device::DeviceCrypto dev;
    auto h = dev.sha256(reinterpret_cast<const uint8_t*>("abc"), 3);
    const uint8_t expect[32] = {  // FIPS 180-2 "abc"
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,
        0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,
        0xf2,0x00,0x15,0xad};
    for (int i = 0; i < 32; ++i) CHECK_EQ(int(h[i]), int(expect[i]));

    std::vector<uint8_t> buf(10000);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = uint8_t(i * 7 + 1);
    host::OpenSslCrypto ossl;
    CHECK(dev.sha256(buf.data(), buf.size()) == ossl.sha256(buf.data(), buf.size()));
}

TEST(DeviceCrypto, VerifiesOpenSslSignature) {
    host::OpenSslCrypto ossl;
    device::DeviceCrypto dev;
    ota::Ed25519PublicKey pub;
    std::array<uint8_t, 32> priv;
    REQUIRE(host::OpenSslCrypto::genKeypair(pub, priv));

    std::string msg = "carsh manifest v1.2.0 :: sha256=deadbeef";
    ota::Ed25519Signature sig;
    REQUIRE(ossl.sign(priv, reinterpret_cast<const uint8_t*>(msg.data()),
                      msg.size(), sig));

    const auto* m = reinterpret_cast<const uint8_t*>(msg.data());
    CHECK(dev.ed25519Verify(pub, m, msg.size(), sig));         // genuine -> ok
    std::string bad = msg; bad.back() ^= 0x20;
    CHECK(!dev.ed25519Verify(pub, reinterpret_cast<const uint8_t*>(bad.data()),
                             bad.size(), sig));                // tampered msg
    ota::Ed25519Signature badSig = sig; badSig[0] ^= 0xFF;
    CHECK(!dev.ed25519Verify(pub, m, msg.size(), badSig));     // tampered sig
}

TEST(DeviceCrypto, FullPackageVerifyWithDeviceBackend) {
    host::OpenSslCrypto ossl;
    device::DeviceCrypto dev;
    ota::Ed25519PublicKey pub;
    std::array<uint8_t, 32> priv;
    REQUIRE(host::OpenSslCrypto::genKeypair(pub, priv));

    std::vector<uint8_t> image(4096, 0x5A);
    auto h = dev.sha256(image.data(), image.size());
    std::string hex;
    const char* d = "0123456789abcdef";
    for (uint8_t b : h) { hex.push_back(d[b >> 4]); hex.push_back(d[b & 15]); }
    std::string js = "{\"version\":\"1.2.0\",\"slotImageSHA256\":\"" + hex +
                     "\",\"minPrevVersion\":\"1.0.0\"}";
    auto carup = ota::buildPackage(js, image, 0xFF,
        [&](const uint8_t* m, std::size_t n, ota::Ed25519Signature& s) {
            ossl.sign(priv, m, n, s);
        });

    // Verify the OpenSSL-signed package with the DEVICE backend.
    ota::Package pkg;
    REQUIRE(ota::parsePackage(carup, pkg));
    CHECK(ota::verifyPackage(pkg, dev, pub) == ota::VerifyResult::Ok);

    carup.back() ^= 0xFF;  // corrupt image
    ota::Package pkg2;
    REQUIRE(ota::parsePackage(carup, pkg2));
    CHECK(ota::verifyPackage(pkg2, dev, pub) == ota::VerifyResult::ImageHashMismatch);
}

#else
TEST(DeviceCrypto, SkippedWithoutOpenSSL) {
    std::printf("  (skip) libcrypto unavailable — device-crypto interop test\n");
}
#endif

// OTA tests: manifest parse, A/B boot state machine, and (with libcrypto) full
// signed-package verification + apply/commit/rollback using real Ed25519.
#include <array>
#include <string>
#include <vector>

#include "Test.h"
#include "ota/BootSlots.h"
#include "ota/Manifest.h"
#include "ota/OtaManager.h"
#include "ota/Package.h"

#ifdef CARSH_HAVE_OPENSSL
#include "crypto/OpenSslCrypto.h"
#include "ota/MemFlashTarget.h"
#endif

using namespace carsh::ota;

// --- Manifest + version (no crypto needed) ----------------------------------

TEST(Ota, VersionParseAndCompare) {
    CHECK_EQ(parseVersion("1.2.3"), 0x010203u);
    CHECK_EQ(parseVersion("0.0.0"), 0u);
    CHECK(parseVersion("1.3.0") > parseVersion("1.2.9"));
    CHECK(parseVersion("2.0.0") > parseVersion("1.99.99"));
}

TEST(Ota, ManifestParse) {
    std::string js =
        "{\"version\":\"1.2.0\",\"buildId\":\"deadbeef\","
        "\"slotImageSHA256\":\"" +
        std::string(64, 'a') +
        "\",\"minPrevVersion\":\"1.0.0\"}";
    Manifest m = parseManifest(reinterpret_cast<const uint8_t*>(js.data()),
                               js.size());
    REQUIRE(m.valid);
    CHECK_EQ(m.version, std::string("1.2.0"));
    CHECK_EQ(m.buildId, std::string("deadbeef"));
    CHECK_EQ(m.minPrevVersion, std::string("1.0.0"));
    CHECK_EQ(int(m.imageSha[0]), 0xAA);
    // Bad hex length -> invalid.
    std::string bad = "{\"version\":\"1\",\"slotImageSHA256\":\"abcd\"}";
    Manifest mb = parseManifest(reinterpret_cast<const uint8_t*>(bad.data()),
                                bad.size());
    CHECK(!mb.valid);
}

// --- A/B boot state machine -------------------------------------------------

TEST(Ota, BootSteadyState) {
    BootRecord r;
    r.activeSlot = 0; r.trying = false; r.healthy = true;
    auto d = decide(r);
    CHECK_EQ(int(d.bootSlot), 0);
    CHECK(!d.rolledBack);
    CHECK(!d.next.trying);
}

TEST(Ota, BootTrialDecrementsThenRollsBack) {
    BootRecord r;
    r.activeSlot = 1;        // trying the new image in slot B
    r.trying = true;
    r.triesLeft = 2;
    // Boot 1: consume an attempt, still booting B.
    auto d1 = decide(r);
    CHECK_EQ(int(d1.bootSlot), 1);
    CHECK_EQ(int(d1.next.triesLeft), 1);
    CHECK(!d1.rolledBack);
    // Boot 2: last attempt.
    auto d2 = decide(d1.next);
    CHECK_EQ(int(d2.bootSlot), 1);
    CHECK_EQ(int(d2.next.triesLeft), 0);
    // Boot 3: exhausted -> roll back to slot A.
    auto d3 = decide(d2.next);
    CHECK_EQ(int(d3.bootSlot), 0);
    CHECK(d3.rolledBack);
    CHECK(!d3.next.trying);
    CHECK(d3.next.healthy);
}

TEST(Ota, ConfirmHealthyStopsTrial) {
    BootRecord r;
    r.activeSlot = 1; r.trying = true; r.triesLeft = 3;
    auto d = decide(r);            // booting the new image
    BootRecord committed = confirmHealthy(d.next);  // app reports healthy
    CHECK(!committed.trying);
    CHECK(committed.healthy);
    // Subsequent boots are steady on slot 1.
    auto d2 = decide(committed);
    CHECK_EQ(int(d2.bootSlot), 1);
    CHECK(!d2.rolledBack);
}

TEST(Ota, StageUpdateTargetsInactiveSlot) {
    BootRecord r;
    r.activeSlot = 0; r.versionA = parseVersion("1.0.0");
    BootRecord n = stageUpdate(r, parseVersion("1.1.0"), 3);
    CHECK_EQ(int(n.activeSlot), 1);          // now booting B
    CHECK(n.trying);
    CHECK_EQ(int(n.triesLeft), 3);
    CHECK_EQ(n.versionB, parseVersion("1.1.0"));
    CHECK_EQ(n.versionA, parseVersion("1.0.0"));  // old slot preserved
}

TEST(Ota, BootRecordSerializeRoundTrip) {
    BootRecord r;
    r.activeSlot = 1; r.trying = true; r.triesLeft = 2; r.healthy = false;
    r.versionA = 0x010002; r.versionB = 0x010100;
    auto blob = serializeBootRecord(r);
    BootRecord o;
    REQUIRE(deserializeBootRecord(blob.data(), blob.size(), o));
    CHECK_EQ(int(o.activeSlot), 1);
    CHECK(o.trying);
    CHECK_EQ(int(o.triesLeft), 2);
    CHECK_EQ(o.versionB, 0x010100u);
}

// --- Full signed package flow (real Ed25519 via OpenSSL) --------------------

#ifdef CARSH_HAVE_OPENSSL

namespace {
// Build a signed .carup for `version` over `image`, signed by `priv`.
std::vector<uint8_t> buildSigned(carsh::host::OpenSslCrypto& crypto,
                                 const std::array<uint8_t, 32>& priv,
                                 const std::string& version,
                                 const std::string& minPrev,
                                 const std::vector<uint8_t>& image,
                                 bool corruptImageHash = false) {
    Sha256Digest h = crypto.sha256(image.data(), image.size());
    std::string hex;
    const char* d = "0123456789abcdef";
    for (uint8_t bb : h) { hex.push_back(d[bb >> 4]); hex.push_back(d[bb & 15]); }
    if (corruptImageHash) hex[0] = (hex[0] == 'a') ? 'b' : 'a';
    std::string js = "{\"version\":\"" + version +
                     "\",\"buildId\":\"test\",\"slotImageSHA256\":\"" + hex +
                     "\",\"minPrevVersion\":\"" + minPrev + "\"}";
    return buildPackage(js, image, 0xFF,
                        [&](const uint8_t* m, std::size_t n,
                            Ed25519Signature& sig) {
                            crypto.sign(priv, m, n, sig);
                        });
}
}  // namespace

TEST(Ota, VerifyGoodPackage) {
    carsh::host::OpenSslCrypto crypto;
    Ed25519PublicKey pub; std::array<uint8_t, 32> priv;
    REQUIRE(carsh::host::OpenSslCrypto::genKeypair(pub, priv));
    std::vector<uint8_t> image(5000, 0x42);
    auto carup = buildSigned(crypto, priv, "1.1.0", "1.0.0", image);

    Package pkg;
    REQUIRE(parsePackage(carup, pkg));
    CHECK(verifyPackage(pkg, crypto, pub) == VerifyResult::Ok);
}

TEST(Ota, RejectsTamperedSignatureAndImage) {
    carsh::host::OpenSslCrypto crypto;
    Ed25519PublicKey pub; std::array<uint8_t, 32> priv;
    REQUIRE(carsh::host::OpenSslCrypto::genKeypair(pub, priv));
    std::vector<uint8_t> image(3000, 0x11);

    // Wrong key -> bad signature.
    Ed25519PublicKey otherPub; std::array<uint8_t, 32> otherPriv;
    carsh::host::OpenSslCrypto::genKeypair(otherPub, otherPriv);
    auto carup = buildSigned(crypto, priv, "1.1.0", "1.0.0", image);
    Package pkg; REQUIRE(parsePackage(carup, pkg));
    CHECK(verifyPackage(pkg, crypto, otherPub) == VerifyResult::BadSignature);

    // Manifest pins a wrong image hash -> mismatch (signature still valid).
    auto bad = buildSigned(crypto, priv, "1.1.0", "1.0.0", image, true);
    Package pkg2; REQUIRE(parsePackage(bad, pkg2));
    CHECK(verifyPackage(pkg2, crypto, pub) == VerifyResult::ImageHashMismatch);

    // Flip a byte of the image after signing -> mismatch.
    carup.back() ^= 0xFF;
    Package pkg3; REQUIRE(parsePackage(carup, pkg3));
    CHECK(verifyPackage(pkg3, crypto, pub) == VerifyResult::ImageHashMismatch);
}

TEST(Ota, ApplyWritesInactiveSlotAndArmsTrial) {
    carsh::host::OpenSslCrypto crypto;
    Ed25519PublicKey pub; std::array<uint8_t, 32> priv;
    REQUIRE(carsh::host::OpenSslCrypto::genKeypair(pub, priv));
    carsh::host::MemFlashTarget flash;
    OtaManager ota(crypto, pub, flash);

    std::vector<uint8_t> image(4096, 0x7E);
    auto carup = buildSigned(crypto, priv, "1.2.0", "1.0.0", image);

    BootRecord cur;
    cur.activeSlot = 0; cur.healthy = true; cur.versionA = parseVersion("1.1.0");
    auto res = ota.apply(carup, cur, parseVersion("1.1.0"), 3);
    REQUIRE(res.ok());
    CHECK_EQ(int(res.writtenSlot), 1);                 // inactive slot
    CHECK(flash.hasSlot(1));
    CHECK_EQ(flash.slot(1).size(), image.size());
    CHECK(res.newRecord.trying);
    CHECK_EQ(int(res.newRecord.activeSlot), 1);
    CHECK_EQ(res.newRecord.versionB, parseVersion("1.2.0"));

    // Commit after a healthy boot.
    BootRecord committed = ota.commit(res.newRecord);
    CHECK(!committed.trying);
    CHECK(committed.healthy);
}

TEST(Ota, ApplyRejectsDowngradeAndOldPrev) {
    carsh::host::OpenSslCrypto crypto;
    Ed25519PublicKey pub; std::array<uint8_t, 32> priv;
    REQUIRE(carsh::host::OpenSslCrypto::genKeypair(pub, priv));
    carsh::host::MemFlashTarget flash;
    OtaManager ota(crypto, pub, flash);
    std::vector<uint8_t> image(1000, 9);

    BootRecord cur;
    cur.activeSlot = 0; cur.versionA = parseVersion("2.0.0");

    // Downgrade (1.0.0 < running 2.0.0) blocked.
    auto down = buildSigned(crypto, priv, "1.0.0", "0.9.0", image);
    CHECK(ota.apply(down, cur, parseVersion("2.0.0")).status ==
          ApplyStatus::DowngradeBlocked);

    // minPrevVersion gate: package requires >= 3.0.0 but we run 2.0.0.
    auto needNewer = buildSigned(crypto, priv, "3.1.0", "3.0.0", image);
    CHECK(ota.apply(needNewer, cur, parseVersion("2.0.0")).status ==
          ApplyStatus::PrevVersionTooOld);

    CHECK(!flash.hasSlot(1));  // nothing written on rejected updates
}

TEST(Ota, ApplyHandlesFlashFailure) {
    carsh::host::OpenSslCrypto crypto;
    Ed25519PublicKey pub; std::array<uint8_t, 32> priv;
    REQUIRE(carsh::host::OpenSslCrypto::genKeypair(pub, priv));
    carsh::host::MemFlashTarget flash;
    flash.failNextWrite();
    OtaManager ota(crypto, pub, flash);
    std::vector<uint8_t> image(1000, 1);
    auto carup = buildSigned(crypto, priv, "1.1.0", "1.0.0", image);
    BootRecord cur; cur.versionA = parseVersion("1.0.0");
    CHECK(ota.apply(carup, cur, parseVersion("1.0.0")).status ==
          ApplyStatus::FlashWriteFailed);
}

#endif  // CARSH_HAVE_OPENSSL

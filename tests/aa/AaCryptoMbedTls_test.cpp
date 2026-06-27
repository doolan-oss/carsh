// AaCryptoMbedTls / AaMemBio tests. The pure in-memory BIO plumbing is host-tested
// unconditionally. When the build server has mbedTLS (the same 2.28 line circle-
// stdlib bundles), the device crypto unit itself is compiled in and driven through
// a real in-memory TLS 1.2 handshake between two instances (head-unit client <->
// scripted server), then encrypt/decrypt round-trips — the exact plumbing the live
// phone exercises [hw]. See AA_PLAN rung 4b.
#include "Test.h"
#include "aa/AaMemBio.h"

using carsh::aa::AaMemBio;

// ---- Pure BIO plumbing (no TLS stack required) ------------------------------
TEST(AaMemBio, PushRecvDrainSemantics) {
    AaMemBio bio;
    uint8_t buf[8];

    // Empty in-buffer reports WANT_READ (mbedTLS's "would block").
    CHECK_EQ(bio.recv(buf, sizeof(buf)), AaMemBio::kWantRead);

    const uint8_t in[] = {1, 2, 3, 4, 5};
    bio.pushIn(in, sizeof(in));
    CHECK_EQ(static_cast<int>(bio.inAvailable()), 5);

    // Partial drain, then the remainder, then back to WANT_READ.
    CHECK_EQ(bio.recv(buf, 3), 3);
    CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 3);
    CHECK_EQ(bio.recv(buf, sizeof(buf)), 2);
    CHECK(buf[0] == 4 && buf[1] == 5);
    CHECK_EQ(bio.recv(buf, sizeof(buf)), AaMemBio::kWantRead);

    // send() accumulates; drainOut() takes and clears.
    CHECK(bio.outEmpty());
    const uint8_t out[] = {0xAA, 0xBB};
    CHECK_EQ(bio.send(out, sizeof(out)), 2);
    CHECK(!bio.outEmpty());
    auto drained = bio.drainOut();
    CHECK_EQ(static_cast<int>(drained.size()), 2);
    CHECK(drained[0] == 0xAA && drained[1] == 0xBB);
    CHECK(bio.outEmpty());
}

#ifdef CARSH_HAVE_MBEDTLS
#include <random>
#include <string>
#include <vector>

#include "AaCryptoMbedTls.h"

using carsh::device::AaCryptoMbedTls;

namespace {

// Host entropy source for seeding the CTR-DRBG (the device supplies the Pi's HW
// RNG instead). Non-deterministic is fine — the test asserts behaviour, not bytes.
int hostEntropy(void*, unsigned char* buf, size_t len) {
    static std::random_device rd;
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<unsigned char>(rd() & 0xFF);
    return 0;
}

// Ping-pong the in-memory handshake to completion. Each side's handshakeStep()
// returns the bytes it wants to transmit; we hand them to the peer. Returns true
// once both report handshakeDone.
bool driveHandshake(AaCryptoMbedTls& cli, AaCryptoMbedTls& srv) {
    std::vector<uint8_t> toSrv = cli.handshakeStep(nullptr, 0);  // ClientHello
    std::vector<uint8_t> toCli;
    for (int i = 0; i < 32 && !(cli.handshakeDone() && srv.handshakeDone()); ++i) {
        toCli = srv.handshakeStep(toSrv.data(), toSrv.size());
        toSrv = cli.handshakeStep(toCli.data(), toCli.size());
        if (toSrv.empty() && toCli.empty() &&
            !(cli.handshakeDone() && srv.handshakeDone())) {
            break;  // no progress and not done -> stuck
        }
    }
    return cli.handshakeDone() && srv.handshakeDone();
}

// Encrypt `msg` on `from`, deliver the resulting record(s) to `to`, and return the
// decrypted plaintext — i.e. one application-data hop across the TLS session.
std::vector<uint8_t> hop(AaCryptoMbedTls& from, AaCryptoMbedTls& to,
                         const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> cipher = from.encrypt(msg.data(), msg.size());
    if (cipher.empty()) return {};
    return to.decrypt(cipher.data(), cipher.size());
}

}  // namespace

TEST(AaCryptoMbedTls, FullHandshakeAndRecordRoundTrip) {
    AaCryptoMbedTls cli(hostEntropy, nullptr);
    AaCryptoMbedTls srv(hostEntropy, nullptr);
    srv.setServer(true);
    REQUIRE(cli.init());
    REQUIRE(srv.init());

    REQUIRE(driveHandshake(cli, srv));
    CHECK(cli.handshakeDone());
    CHECK(srv.handshakeDone());

    // Application data both directions (AA wraps each channel message in a record).
    std::vector<uint8_t> up = {0x00, 0x06, 'h', 'e', 'l', 'l', 'o'};  // [msgId][body]
    CHECK(hop(cli, srv, up) == up);

    std::vector<uint8_t> down(300);
    for (size_t i = 0; i < down.size(); ++i) down[i] = static_cast<uint8_t>(i * 7 + 1);
    CHECK(hop(srv, cli, down) == down);
}

// The real aasdk head-unit cert must take the raw-cert bypass (its non-standard
// UTCTime fails mbedTLS's x509 parse). Force that exact path for a cert that would
// otherwise parse and confirm the handshake + records still complete — so the code
// the bench relies on is host-proven, even though the bench supplies the real cert.
TEST(AaCryptoMbedTls, RawCertBypassStillHandshakes) {
    AaCryptoMbedTls cli(hostEntropy, nullptr);
    AaCryptoMbedTls srv(hostEntropy, nullptr);
    cli.setForceRawCert(true);  // head unit presents its cert via raw bytes
    srv.setServer(true);
    REQUIRE(cli.init());
    REQUIRE(srv.init());

    REQUIRE(driveHandshake(cli, srv));

    std::vector<uint8_t> msg = {0x00, 0x05, 1, 2, 3, 4};
    CHECK(hop(cli, srv, msg) == msg);
}

// Before the handshake completes, encrypt/decrypt must no-op (return empty) rather
// than emit unprotected bytes — AaConnection relies on this to gate encrypted sends.
TEST(AaCryptoMbedTls, NoRecordsBeforeHandshakeDone) {
    AaCryptoMbedTls cli(hostEntropy, nullptr);
    REQUIRE(cli.init());
    CHECK(!cli.handshakeDone());
    const uint8_t data[] = {1, 2, 3};
    CHECK(cli.encrypt(data, sizeof(data)).empty());
    CHECK(cli.decrypt(data, sizeof(data)).empty());
}
#endif  // CARSH_HAVE_MBEDTLS

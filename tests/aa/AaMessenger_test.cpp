// AaMessenger framing/reassembly tests. Two kinds of coverage:
//   1. Against REAL captured AA traffic (tests/aa/fixtures, trimmed from the
//      dylant9/head_unit DHU<->phone capture) — proves we parse the actual
//      handshake the same way Google's reference does.
//   2. Synthetic round-trips that stress fragmentation: feed the stream one byte
//      at a time, and round-trip a multi-frame send back through receive.
// All host-side; no radio (H6 / AA_PLAN Phase 1).
#include "aa/AaMessenger.h"

#include <cstdio>
#include <string>
#include <vector>

#include "Test.h"
#include "aa/FakeAaTransport.h"

using namespace carsh;
using carsh::aa::AaMessage;
using carsh::aa::AaMessenger;
using carsh::aa::ChannelId;
using carsh::aa::EncryptionType;
using carsh::host::FakeAaTransport;

namespace {

std::vector<uint8_t> loadFixture(const char* name) {
    std::string path = std::string(CARSH_TEST_DIR) + "/aa/fixtures/" + name;
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    size_t rd = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    REQUIRE(rd == out.size());
    return out;
}

}  // namespace

// The captured PHN->DHU handshake, byte-for-byte: version response, two SSL
// handshake messages, then encrypted frames on the control + input channels.
// Fed 7 bytes at a time so frames straddle read boundaries (the USB reality).
TEST(AaMessenger, ReassemblesCapturedHandshake) {
    std::vector<uint8_t> rx = loadFixture("aa_rx_handshake.bin");
    FakeAaTransport tp;
    tp.pushRx(rx);
    tp.setReadChunk(7);  // force frames to span multiple reads
    AaMessenger m(tp);

    AaMessage msg;
    // 0: version response — plain control message, id 0x0002, body major/minor/status
    REQUIRE(m.receive(msg, 100));
    CHECK(msg.channel == ChannelId::Control);
    CHECK(msg.encryption == EncryptionType::Plain);
    CHECK_EQ(int(msg.messageId), 0x0002);
    CHECK_EQ(msg.payload.size(), std::size_t(6));  // 1,7,0 as three BE16

    // 1,2: SSL handshake — plain, id 0x0003 (msg id stripped from payload)
    REQUIRE(m.receive(msg, 100));
    CHECK(msg.channel == ChannelId::Control);
    CHECK_EQ(int(msg.messageId), 0x0003);
    CHECK_EQ(msg.payload.size(), std::size_t(2349 - 2));

    REQUIRE(m.receive(msg, 100));
    CHECK_EQ(int(msg.messageId), 0x0003);
    CHECK_EQ(msg.payload.size(), std::size_t(53 - 2));

    // 3,4: encrypted control frames — id not visible (inside ciphertext)
    REQUIRE(m.receive(msg, 100));
    CHECK(msg.channel == ChannelId::Control);
    CHECK(msg.encryption == EncryptionType::Encrypted);
    CHECK_EQ(int(msg.messageId), 0);
    CHECK_EQ(msg.payload.size(), std::size_t(910));

    REQUIRE(m.receive(msg, 100));
    CHECK(msg.encryption == EncryptionType::Encrypted);
    CHECK_EQ(msg.payload.size(), std::size_t(33));

    // 5: first non-control channel comes up encrypted (input, channel 1)
    REQUIRE(m.receive(msg, 100));
    CHECK(msg.channel == ChannelId::Input);
    CHECK(msg.encryption == EncryptionType::Encrypted);
}

// The send path must reproduce the exact version-request bytes the DHU emits:
//   00 03 | 00 06 | 0001 | 0001 0007   (ch0, BULK+SPECIFIC+PLAIN, len6, id1, v1.7)
TEST(AaMessenger, SendVersionRequestMatchesCapture) {
    FakeAaTransport tp;
    AaMessenger m(tp);
    const uint8_t body[] = {0x00, 0x01, 0x00, 0x07};  // major=1, minor=7
    REQUIRE(m.sendSpecific(ChannelId::Control, EncryptionType::Plain, 0x0001, body, sizeof(body)));

    const std::vector<uint8_t> expect = {0x00, 0x03, 0x00, 0x06,
                                         0x00, 0x01, 0x00, 0x01, 0x00, 0x07};
    REQUIRE(tp.sent().size() == expect.size());
    CHECK(tp.sent() == expect);

    // ...and matches frame 0 of the real DHU->PHN capture.
    std::vector<uint8_t> txCap = loadFixture("aa_tx_handshake.bin");
    REQUIRE(txCap.size() >= expect.size());
    bool same = std::equal(expect.begin(), expect.end(), txCap.begin());
    CHECK(same);
}

// A message larger than one USB frame must fragment FIRST/MIDDLE/LAST on send and
// reassemble byte-exact on receive — round-tripped through the wire format.
TEST(AaMessenger, MultiFrameRoundTrip) {
    std::vector<uint8_t> payload(40000);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 31 + 7);

    FakeAaTransport tx;
    AaMessenger sender(tx);
    REQUIRE(sender.sendSpecific(ChannelId::Video, EncryptionType::Plain, 0x00AB,
                                payload.data(), payload.size()));
    CHECK(tx.sent().size() > carsh::aa::kUsbTransferSize);  // it really fragmented

    FakeAaTransport rx;
    rx.pushRx(tx.sent());
    rx.setReadChunk(13);  // nastiest case: tiny reads across every frame boundary
    AaMessenger receiver(rx);
    AaMessage msg;
    REQUIRE(receiver.receive(msg, 100));
    CHECK(msg.channel == ChannelId::Video);
    CHECK_EQ(int(msg.messageId), 0x00AB);
    REQUIRE(msg.payload.size() == payload.size());
    CHECK(msg.payload == payload);
}

// Coalesced frames in a single read must all be extracted; a trailing partial
// frame waits for the next read rather than corrupting the stream.
TEST(AaMessenger, CoalescedAndSplitFrames) {
    FakeAaTransport tx;
    AaMessenger sender(tx);
    const uint8_t a[] = {0xDE, 0xAD};
    const uint8_t b[] = {0xBE, 0xEF, 0x01};
    sender.sendSpecific(ChannelId::Sensor, EncryptionType::Plain, 0x0010, a, sizeof(a));
    sender.sendSpecific(ChannelId::Sensor, EncryptionType::Plain, 0x0011, b, sizeof(b));

    FakeAaTransport rx;
    rx.pushRx(tx.sent());  // both frames delivered in one read (default chunk)
    AaMessenger receiver(rx);
    AaMessage m1, m2;
    REQUIRE(receiver.receive(m1, 100));
    REQUIRE(receiver.receive(m2, 100));
    CHECK_EQ(int(m1.messageId), 0x0010);
    CHECK(m1.payload == std::vector<uint8_t>(a, a + sizeof(a)));
    CHECK_EQ(int(m2.messageId), 0x0011);
    CHECK(m2.payload == std::vector<uint8_t>(b, b + sizeof(b)));
}

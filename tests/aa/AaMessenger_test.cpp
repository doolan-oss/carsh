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

namespace {

using carsh::aa::FrameType;
using carsh::aa::MessageType;

// Hand-build one raw frame: [ch][flags][size BE]([total BE] on FIRST)[payload].
std::vector<uint8_t> rawFrame(uint8_t ch, FrameType ft, EncryptionType enc,
                              const std::vector<uint8_t>& payload, uint32_t total = 0) {
    std::vector<uint8_t> f = {ch, carsh::aa::frameFlags(ft, MessageType::Specific, enc),
                              static_cast<uint8_t>(payload.size() >> 8),
                              static_cast<uint8_t>(payload.size() & 0xFF)};
    if (ft == FrameType::First) {
        f.push_back(static_cast<uint8_t>(total >> 24));
        f.push_back(static_cast<uint8_t>(total >> 16));
        f.push_back(static_cast<uint8_t>(total >> 8));
        f.push_back(static_cast<uint8_t>(total));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

}  // namespace

// TLS records are sequence-numbered, so EVERY encrypted frame must reach the
// decryptor, in order — including frames reassembly then throws away (unknown
// channel, orphan MIDDLE/LAST, the rest of an oversized message). Skipping one used
// to desync TLS and silently wedge the session. Only the plaintext is discarded.
TEST(AaMessenger, EveryEncryptedFrameIsDecryptedInOrder) {
    const auto E = EncryptionType::Encrypted;
    std::vector<uint8_t> wire;
    append(wire, rawFrame(12, FrameType::Bulk, E, {0x01, 0x00, 0x05}));   // unknown ch
    append(wire, rawFrame(3, FrameType::Middle, E, {0x02, 0xAA}));        // orphan MIDDLE
    append(wire, rawFrame(3, FrameType::Last, E, {0x03, 0xBB}));          // orphan LAST
    append(wire, rawFrame(3, FrameType::First, E, {0x04, 0x80, 0x01},     // oversized FIRST
                          static_cast<uint32_t>(carsh::aa::kMaxMessageSize) + 1));
    append(wire, rawFrame(3, FrameType::Middle, E, {0x05, 0xCC}));        // ...its MIDDLE
    append(wire, rawFrame(3, FrameType::Last, E, {0x06, 0xDD}));          // ...its LAST
    append(wire, rawFrame(1, FrameType::Bulk, E, {0x07, 0x80, 0x01, 0x42}));  // valid

    FakeAaTransport tp;
    tp.pushRx(wire);
    tp.setReadChunk(5);
    AaMessenger m(tp);
    std::vector<uint8_t> order;  // first byte of each record the decryptor saw
    m.setDecryptor([&](const uint8_t* p, size_t n) {
        order.push_back(p[0]);
        return std::vector<uint8_t>(p + 1, p + n);  // strip the "sequence" byte
    });

    AaMessage msg;
    REQUIRE(m.receive(msg, 0));  // only the valid message comes out...
    CHECK(msg.channel == ChannelId::Input);
    CHECK(msg.decrypted);
    CHECK_EQ(int(msg.messageId), 0x8001);
    CHECK(msg.payload == std::vector<uint8_t>{0x42});
    CHECK(!m.receive(msg, 0));
    // ...but all seven records were decrypted, in arrival order.
    CHECK(order == (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7}));
    CHECK_EQ(m.droppedFrames(), 6u);
}

// A decrypt that yields nothing (failed record) must not surface as a message with
// id 0 — on an AV channel that used to read as a malformed MEDIA_WITH_TIMESTAMP and
// draw a spurious ack.
TEST(AaMessenger, EmptyPlaintextIsDropped) {
    const auto E = EncryptionType::Encrypted;
    std::vector<uint8_t> wire;
    append(wire, rawFrame(3, FrameType::Bulk, E, {0x00, 0x11}));        // decrypts to {}
    append(wire, rawFrame(3, FrameType::Bulk, E, {0x01, 0x80, 0x01}));  // valid
    FakeAaTransport tp;
    tp.pushRx(wire);
    AaMessenger m(tp);
    m.setDecryptor([](const uint8_t* p, size_t n) {
        if (p[0] == 0x00) return std::vector<uint8_t>{};
        return std::vector<uint8_t>(p + 1, p + n);
    });
    AaMessage msg;
    REQUIRE(m.receive(msg, 0));
    CHECK_EQ(int(msg.messageId), 0x8001);
    CHECK(!m.receive(msg, 0));
    CHECK_EQ(m.droppedFrames(), 1u);
}

// kMaxMessageSize bounds the ACCUMULATED reassembly, not just FIRST's declared total:
// a peer that declares 4 KB and then keeps sending 64 KB MIDDLE frames used to grow
// the buffer without limit (4x the cap in the review probe). The message is dropped
// once it would pass the cap, its remaining frames are orphans, and the channel then
// reassembles the next message normally.
TEST(AaMessenger, ReassemblyIsCappedAtMaxMessageSize) {
    const auto P = EncryptionType::Plain;
    const std::vector<uint8_t> big(0xFFFF, 0x5A);
    const size_t middles = carsh::aa::kMaxMessageSize / big.size() + 2;  // > the cap
    std::vector<uint8_t> wire;
    append(wire, rawFrame(3, FrameType::First, P, {0x00, 0x01, 0x00}, 4096));
    for (size_t i = 0; i < middles; ++i) append(wire, rawFrame(3, FrameType::Middle, P, big));
    append(wire, rawFrame(3, FrameType::Last, P, {0x01}));
    append(wire, rawFrame(3, FrameType::Bulk, P, {0x00, 0x01, 0x77}));  // next message

    FakeAaTransport tp;
    tp.pushRx(wire);
    AaMessenger m(tp);
    AaMessage msg;
    size_t messages = 0, largest = 0;
    while (m.receive(msg, 0)) {
        ++messages;
        if (msg.payload.size() > largest) largest = msg.payload.size();
    }
    CHECK_EQ(messages, size_t(1));  // only the trailing BULK survives
    CHECK(largest <= carsh::aa::kMaxMessageSize);
    CHECK_EQ(int(msg.messageId), 0x0001);
    CHECK(msg.payload == std::vector<uint8_t>{0x77});
    CHECK(m.droppedFrames() >= 2u);  // the capped message + the orphan tail
}

// Same bound on the encrypted path, and the cap never skips the decryptor.
TEST(AaMessenger, EncryptedReassemblyCapStillDecryptsEveryFrame) {
    const auto E = EncryptionType::Encrypted;
    const std::vector<uint8_t> big(0xFFFF, 0x5A);
    const size_t middles = carsh::aa::kMaxMessageSize / big.size() + 2;
    std::vector<uint8_t> wire;
    append(wire, rawFrame(3, FrameType::First, E, {0x00, 0x01, 0x00}, 4096));
    for (size_t i = 0; i < middles; ++i) append(wire, rawFrame(3, FrameType::Middle, E, big));
    append(wire, rawFrame(3, FrameType::Last, E, {0x01}));
    append(wire, rawFrame(3, FrameType::Bulk, E, {0x80, 0x01, 0x66}));

    FakeAaTransport tp;
    tp.pushRx(wire);
    AaMessenger m(tp);
    size_t records = 0;
    m.setDecryptor([&](const uint8_t* p, size_t n) {
        ++records;
        return std::vector<uint8_t>(p, p + n);
    });
    AaMessage msg;
    size_t messages = 0;
    while (m.receive(msg, 0)) ++messages;
    CHECK_EQ(messages, size_t(1));
    CHECK_EQ(int(msg.messageId), 0x8001);
    CHECK_EQ(records, middles + 3);  // FIRST + every MIDDLE + LAST + the BULK
}

// AaBulkLink — the RX ring + sticky dead latch under the device AOA transport
// (device/CircleAoaTransport). Pins the review A1/A3/A7 behaviour: a failed bulk-IN
// transfer is never asked for its length, latches the link dead and is never
// re-posted; an RX overflow or a failed bulk-OUT is just as fatal; a dead link
// refuses read()/write() so the AA stack sees "disconnect". A LinkTransport wrapper
// mirrors the device glue so AaMessenger/AaConnection can be driven over it.
#include "aa/AaBulkLink.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "Test.h"
#include "aa/AaConnection.h"
#include "aa/AaMessenger.h"
#include "aa/FakeAaCrypto.h"
#include "aa/IAaTransport.h"

using carsh::aa::AaBulkLink;
using carsh::aa::AaLinkFault;

namespace {

std::vector<uint8_t> seq(size_t n, uint8_t start = 0) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(start + i);
    return v;
}

// The device glue in miniature: read() drains the link, write() refuses once dead
// and otherwise "sends" (recording the bytes) or fails like a timed-out bulk-OUT.
template <size_t N>
class LinkTransport : public carsh::aa::IAaTransport {
public:
    AaBulkLink<N> link;
    std::vector<uint8_t> sent;
    bool failNextWrite = false;
    int writeAttempts = 0;  // writes that reached the (fake) USB pipe

    int read(uint8_t* buf, size_t len, uint32_t) override { return link.read(buf, len); }
    int write(const uint8_t* data, size_t len, uint32_t) override {
        if (!link.writable()) return -1;
        ++writeAttempts;
        if (failNextWrite) {
            failNextWrite = false;
            link.onOutFailed();
            return -1;
        }
        sent.insert(sent.end(), data, data + len);
        link.onOutComplete(len);
        return static_cast<int>(len);
    }
};

}  // namespace

TEST(AaBulkLink, SuccessfulInQueuesBytesAndRearms) {
    AaBulkLink<64> link;
    const auto a = seq(10, 1);
    CHECK(link.onInComplete(true, a.data(), a.size()));  // re-post
    CHECK_EQ(link.ringUsed(), size_t(10));
    CHECK_EQ(link.rxBytes(), uint32_t(10));
    uint8_t out[32] = {};
    CHECK_EQ(link.read(out, 4), 4);  // partial drain
    CHECK_EQ(int(out[0]), 1);
    CHECK_EQ(int(out[3]), 4);
    CHECK_EQ(link.read(out, sizeof out), 6);
    CHECK_EQ(int(out[5]), 10);
    CHECK_EQ(link.read(out, sizeof out), 0);  // empty -> "timeout", not an error
    CHECK(!link.dead());
    CHECK(link.fault() == AaLinkFault::None);
}

TEST(AaBulkLink, ZeroLengthPacketRearmsWithoutQueueing) {
    AaBulkLink<16> link;
    CHECK(link.onInComplete(true, nullptr, 0));
    CHECK_EQ(link.ringUsed(), size_t(0));
    CHECK(!link.dead());
}

// A1: a failed URB has no valid length on Circle's xHCI path (reading it asserts in
// the ISR). The link must not look at data/n at all, must latch dead, must say "don't
// re-post" (A3: the endpoint is halted), and must report disconnect to the reader.
TEST(AaBulkLink, FailedInLatchesDeadWithoutTouchingLength) {
    AaBulkLink<64> link;
    const auto a = seq(8);
    CHECK(link.onInComplete(true, a.data(), a.size()));
    CHECK(!link.onInComplete(false, nullptr, size_t(-1)));  // garbage len: ignored
    CHECK(link.dead());
    CHECK(link.fault() == AaLinkFault::InError);
    CHECK_EQ(link.rxBytes(), uint32_t(8));  // nothing added by the failed transfer
    uint8_t out[16];
    CHECK_EQ(link.read(out, sizeof out), -1);  // pre-fault bytes belong to a dead session
    CHECK(!link.writable());
}

// Once dead, the other (still posted) request may complete normally — it must not be
// queued nor re-posted, so the IN pipe drains to zero requests instead of streaming
// into a session that is already over.
TEST(AaBulkLink, DeadLinkNeverRearmsOrQueuesLateData) {
    AaBulkLink<64> link;
    link.onOutFailed();
    const auto a = seq(5);
    CHECK(!link.onInComplete(true, a.data(), a.size()));
    CHECK_EQ(link.ringUsed(), size_t(0));
    CHECK_EQ(link.rxBytes(), uint32_t(0));
    CHECK(link.fault() == AaLinkFault::OutError);
}

// Dropping the tail of a transfer silently desyncs AA framing + the TLS sequence, so
// a transfer that doesn't fit kills the link instead — whole transfer or nothing.
TEST(AaBulkLink, OverflowIsFatalAndAllOrNothing) {
    AaBulkLink<16> link;  // capacity 15
    const auto a = seq(10);
    CHECK(link.onInComplete(true, a.data(), a.size()));
    const auto b = seq(6, 100);
    CHECK(!link.onInComplete(true, b.data(), b.size()));
    CHECK(link.fault() == AaLinkFault::RxOverflow);
    CHECK_EQ(link.rxBytes(), uint32_t(10));  // none of b was accepted
    CHECK_EQ(link.ringUsed(), size_t(10));
    uint8_t out[16];
    CHECK_EQ(link.read(out, sizeof out), -1);
}

TEST(AaBulkLink, ExactCapacityFitsAndWraps) {
    AaBulkLink<16> link;
    uint8_t out[16];
    for (int round = 0; round < 5; ++round) {
        const auto a = seq(15, static_cast<uint8_t>(round * 15));
        REQUIRE(link.onInComplete(true, a.data(), a.size()));
        CHECK_EQ(link.ringUsed(), size_t(15));
        REQUIRE(link.read(out, sizeof out) == 15);
        for (int i = 0; i < 15; ++i) CHECK_EQ(int(out[i]), (round * 15 + i) & 0xFF);
    }
    CHECK(!link.dead());
}

TEST(AaBulkLink, OddSizesAcrossTheWrapKeepOrder) {
    AaBulkLink<32> link;
    uint8_t next = 0, expect = 0;
    uint8_t out[32];
    for (int i = 0; i < 200; ++i) {
        const size_t n = 1 + (i * 7) % 13;
        // Drain in odd-sized reads until the next transfer fits (it always must —
        // an overflow is fatal by design, see OverflowIsFatalAndAllOrNothing).
        while (link.kCapacity - link.ringUsed() < n) {
            const int got = link.read(out, 1 + (i * 5) % 11);
            REQUIRE(got > 0);
            for (int k = 0; k < got; ++k) CHECK_EQ(int(out[k]), int(expect++));
        }
        std::vector<uint8_t> v(n);
        for (auto& b : v) b = next++;
        REQUIRE(link.onInComplete(true, v.data(), v.size()));
    }
    int got;
    while ((got = link.read(out, sizeof out)) > 0)
        for (int k = 0; k < got; ++k) CHECK_EQ(int(out[k]), int(expect++));
    CHECK_EQ(int(expect), int(next));
}

TEST(AaBulkLink, RearmFailureAndOutStallAreFatal) {
    AaBulkLink<16> a;
    a.onInRearmFailed();
    CHECK(a.fault() == AaLinkFault::InRearmFailed);
    CHECK(!a.writable());
    AaBulkLink<16> b;
    b.onOutStalled();
    CHECK(b.fault() == AaLinkFault::OutStalled);
    CHECK(!b.onInComplete(true, nullptr, 0));
}

TEST(AaBulkLink, FirstFaultWins) {
    AaBulkLink<16> link;
    CHECK(!link.onInComplete(false, nullptr, 0));
    link.onOutFailed();
    link.onInRearmFailed();
    CHECK(link.fault() == AaLinkFault::InError);
}

TEST(AaBulkLink, FaultNamesAreDistinct) {
    CHECK(std::string(carsh::aa::aaLinkFaultName(AaLinkFault::InError)) !=
          carsh::aa::aaLinkFaultName(AaLinkFault::OutError));
    CHECK(std::string(carsh::aa::aaLinkFaultName(AaLinkFault::None)) == "none");
}

// The AA stack over a dead link: the messenger reports disconnect instead of hanging
// on half a frame, and a failed bulk-OUT stops every later write from reaching USB
// (A7: never submit another OUT on a pipe whose last TRB may still be live).
TEST(AaBulkLink, MessengerSeesDisconnectAndWritesStopAfterOutFailure) {
    LinkTransport<1024> t;
    carsh::aa::AaMessenger m(t);
    const uint8_t body[3] = {1, 2, 3};
    CHECK(m.send(carsh::aa::ChannelId::Control, carsh::aa::EncryptionType::Plain,
                 carsh::aa::MessageType::Specific, 0x0001, body, sizeof body));
    CHECK_EQ(t.writeAttempts, 1);
    t.failNextWrite = true;
    CHECK(!m.send(carsh::aa::ChannelId::Control, carsh::aa::EncryptionType::Plain,
                  carsh::aa::MessageType::Specific, 0x0001, body, sizeof body));
    CHECK(t.link.fault() == AaLinkFault::OutError);
    CHECK(!m.send(carsh::aa::ChannelId::Control, carsh::aa::EncryptionType::Plain,
                  carsh::aa::MessageType::Specific, 0x0001, body, sizeof body));
    CHECK_EQ(t.writeAttempts, 2);  // the third send never reached the pipe
    carsh::aa::AaMessage msg;
    CHECK(!m.receive(msg, 0));
}

// A bulk-IN error in the middle of the version exchange: the connection stops
// making progress (poll false) and nothing more goes out on the dead link.
TEST(AaBulkLink, ConnectionGoesQuietOnInError) {
    LinkTransport<4096> t;
    carsh::aa::AaMessenger m(t);
    carsh::host::FakeAaCrypto crypto;
    carsh::aa::AaConnection conn(m, crypto, {});
    conn.start();  // VERSION_REQUEST
    CHECK(!t.sent.empty());
    const size_t sentBefore = t.sent.size();
    // Half a frame arrives, then the transfer error.
    const uint8_t half[3] = {0x00, 0x03, 0x00};
    CHECK(t.link.onInComplete(true, half, sizeof half));
    CHECK(!t.link.onInComplete(false, nullptr, 0));
    CHECK(!conn.poll(0));
    CHECK(!conn.poll(0));
    CHECK_EQ(t.sent.size(), sentBefore);
    CHECK(t.link.dead());
}

// Genuine concurrency (the IRQ producer vs the task consumer): every byte arrives
// once, in order. The producer only offers what fits, as the xHCI buffers would.
TEST(AaBulkLink, SpscStressKeepsOrder) {
    static AaBulkLink<4096> link;
    constexpr uint32_t kTotal = 4u * 1024 * 1024;
    std::atomic<bool> bad{false};
    std::thread producer([&] {
        uint32_t sent = 0;
        uint8_t chunk[700];
        uint32_t k = 0;
        while (sent < kTotal) {
            const size_t n = 1 + (k++ * 131) % sizeof chunk;
            const size_t take = n < kTotal - sent ? n : kTotal - sent;
            if (link.kCapacity - link.ringUsed() < take) {
                std::this_thread::yield();
                continue;
            }
            for (size_t i = 0; i < take; ++i) chunk[i] = static_cast<uint8_t>(sent + i);
            if (!link.onInComplete(true, chunk, take)) {
                bad.store(true);
                return;
            }
            sent += static_cast<uint32_t>(take);
        }
    });
    uint32_t got = 0;
    uint8_t buf[997];
    while (got < kTotal && !bad.load()) {
        const int n = link.read(buf, sizeof buf);
        if (n < 0) {
            bad.store(true);
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (buf[i] != static_cast<uint8_t>(got + i)) bad.store(true);
        }
        got += static_cast<uint32_t>(n);
        if (n == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(!bad.load());
    CHECK_EQ(got, kTotal);
    CHECK(!link.dead());
}

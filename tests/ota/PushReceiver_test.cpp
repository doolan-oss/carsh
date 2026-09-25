// PushReceiver tests — the serial OTA push framing (BEGIN/DATA/END + CRC32,
// resync on garbage, restart on a fresh BEGIN). The encoder in PushProtocol.h is
// the reference sender the host script mirrors, so these tests pin the wire
// format for both sides.
#include <cstdint>
#include <numeric>
#include <vector>

#include "Test.h"
#include "ota/PushProtocol.h"

using namespace carsh::ota;

namespace {

std::vector<uint8_t> patternBlob(std::size_t n) {
    std::vector<uint8_t> b(n);
    uint32_t x = 0x12345678;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;  // xorshift32
        b[i] = uint8_t(x);
    }
    return b;
}

}  // namespace

TEST(PushReceiver, Crc32MatchesKnownVector) {
    // zlib's crc32("123456789") == 0xCBF43926 — pins polynomial + reflection.
    const char* s = "123456789";
    CHECK_EQ(crc32Of(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(PushReceiver, RoundTripSingleFeed) {
    const auto blob = patternBlob(100000);  // ~25 chunks
    const auto wire = push::encodeTransfer(blob);
    PushReceiver rx;
    rx.feed(wire.data(), wire.size());
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK_EQ(rx.bytesReceived(), blob.size());
    CHECK(rx.takeBlob() == blob);
    CHECK(rx.state() == PushReceiver::State::Idle);  // takeBlob resets
}

TEST(PushReceiver, RoundTripByteDribble) {
    const auto blob = patternBlob(10000);
    const auto wire = push::encodeTransfer(blob, 1000);
    PushReceiver rx;
    for (uint8_t byte : wire) rx.feed(&byte, 1);  // worst-case fragmentation
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushReceiver, EmptyBlobAndOddChunkSizes) {
    for (std::size_t size : {std::size_t(0), std::size_t(1), std::size_t(4095),
                             std::size_t(4096), std::size_t(4097)}) {
        const auto blob = patternBlob(size);
        const auto wire = push::encodeTransfer(blob);
        PushReceiver rx;
        rx.feed(wire.data(), wire.size());
        REQUIRE(rx.state() == PushReceiver::State::Complete);
        CHECK(rx.takeBlob() == blob);
    }
}

TEST(PushReceiver, ResyncsPastLeadingGarbage) {
    const auto blob = patternBlob(5000);
    std::vector<uint8_t> wire = {'l', 'o', 'g', ' ', 'n', 'o', 'i', 's',
                                 'e', 'C', 'P', 'S'};  // incl. a partial magic
    const auto t = push::encodeTransfer(blob);
    wire.insert(wire.end(), t.begin(), t.end());
    PushReceiver rx;
    rx.feed(wire.data(), wire.size());
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
    // (skips counted before the reset in takeBlob; just proves it completed)
}

TEST(PushReceiver, MagicInsidePayloadSurvives) {
    // A blob stuffed with the magic itself must not derail framing.
    std::vector<uint8_t> blob;
    for (int i = 0; i < 3000; ++i)
        blob.insert(blob.end(), {'C', 'P', 'S', 'H'});
    const auto wire = push::encodeTransfer(blob, 100);
    PushReceiver rx;
    rx.feed(wire.data(), wire.size());
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushReceiver, CorruptChunkFailsBlobCrc) {
    const auto blob = patternBlob(9000);
    auto wire = push::encodeTransfer(blob, 1000);
    // Flip a payload byte AND fix up that packet's CRC so only the whole-blob
    // check can catch it (simulates a bad sender, not line noise).
    const std::size_t pkt1 = push::kHeaderLen + 8 + 4;  // after BEGIN
    const std::size_t payloadOff = pkt1 + push::kHeaderLen;
    wire[payloadOff] ^= 0xFF;
    const uint32_t fixed =
        crc32Of(wire.data() + pkt1 + 4, push::kHeaderLen - 4 + 1000);
    for (int i = 0; i < 4; ++i)
        wire[pkt1 + push::kHeaderLen + 1000 + i] = uint8_t(fixed >> (8 * i));
    PushReceiver rx;
    rx.feed(wire.data(), wire.size());
    CHECK(rx.state() == PushReceiver::State::Failed);
    CHECK(rx.error() == PushReceiver::Error::BlobCrcMismatch);
}

TEST(PushReceiver, LineNoiseCorruptionSkipsPacketThenFailsSeq) {
    const auto blob = patternBlob(9000);
    auto wire = push::encodeTransfer(blob, 1000);
    // Corrupt one payload byte without fixing the packet CRC: the receiver must
    // not accept the chunk; it resyncs and the next chunk trips BadSeq.
    wire[push::kHeaderLen + 8 + 4 + push::kHeaderLen + 3] ^= 0x55;
    PushReceiver rx;
    rx.feed(wire.data(), wire.size());
    CHECK(rx.state() == PushReceiver::State::Failed);
    CHECK(rx.error() == PushReceiver::Error::BadSeq);
    CHECK(rx.resyncSkips() > 0);
}

TEST(PushReceiver, FreshBeginRestartsMidTransfer) {
    const auto blobA = patternBlob(8000);
    const auto blobB = patternBlob(3000);
    const auto wireA = push::encodeTransfer(blobA, 1000);
    const auto wireB = push::encodeTransfer(blobB, 1000);
    PushReceiver rx;
    // Half of transfer A (cut mid-stream), then all of B: B must win.
    rx.feed(wireA.data(), wireA.size() / 2);
    CHECK(rx.state() == PushReceiver::State::Receiving);
    rx.feed(wireB.data(), wireB.size());
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blobB);
}

TEST(PushReceiver, RejectsOversizedAnnouncement) {
    PushReceiver rx(1024);  // 1 KB cap
    const auto begin = push::encodeBegin(2048, 0);
    rx.feed(begin.data(), begin.size());
    CHECK(rx.state() == PushReceiver::State::Failed);
    CHECK(rx.error() == PushReceiver::Error::TooLarge);
    // reset() recovers.
    rx.reset();
    const auto blob = patternBlob(512);
    const auto wire = push::encodeTransfer(blob);
    rx.feed(wire.data(), wire.size());
    CHECK(rx.state() == PushReceiver::State::Complete);
}

TEST(PushReceiver, StrayDataBeforeBeginIgnored) {
    const auto blob = patternBlob(2000);
    PushReceiver rx;
    // A valid DATA packet with no transfer open: ignored, stays Idle.
    const auto stray = push::encodePacket(push::kTypeData, 1, blob.data(), 100);
    rx.feed(stray.data(), stray.size());
    CHECK(rx.state() == PushReceiver::State::Idle);
    // Then a clean transfer works.
    const auto wire = push::encodeTransfer(blob);
    rx.feed(wire.data(), wire.size());
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushReceiver, TruncatedTransferReportsSizeMismatchOnEnd) {
    const auto blob = patternBlob(5000);
    PushReceiver rx;
    const auto begin =
        push::encodeBegin(uint32_t(blob.size()), crc32Of(blob.data(), blob.size()));
    rx.feed(begin.data(), begin.size());
    const auto d1 = push::encodePacket(push::kTypeData, 1, blob.data(), 1000);
    rx.feed(d1.data(), d1.size());
    const auto end = push::encodePacket(push::kTypeEnd, 2, nullptr, 0);
    rx.feed(end.data(), end.size());
    CHECK(rx.state() == PushReceiver::State::Failed);
    CHECK(rx.error() == PushReceiver::Error::SizeMismatch);
}

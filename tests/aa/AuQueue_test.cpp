// AuQueue + AsyncH264Decoder — the seam that moves H.264 decode off the AA audio-
// refill path (AA_AUDIO_INVESTIGATION.md fix #3). These tests pin the framed-SPSC
// behaviour (whole-AU push/pop, drop-on-full, counters) and that the async wrapper
// only enqueues on feed() and defers the real decode to pump().
#include <cstdint>
#include <vector>

#include "Test.h"
#include "aa/AsyncH264Decoder.h"
#include "aa/AuQueue.h"
#include "aa/IH264Decoder.h"

using carsh::aa::AsyncH264Decoder;
using carsh::aa::AuQueue;

namespace {
std::vector<std::uint8_t> au(std::uint8_t tag, std::size_t len) {
    std::vector<std::uint8_t> v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = std::uint8_t(tag + i);
    return v;
}

// Records every AU it is fed (stands in for the openh264 decoder on the pump side).
class RecordingDecoder : public carsh::aa::IH264Decoder {
public:
    std::vector<std::vector<std::uint8_t>> fed;
    void feed(const std::uint8_t* p, std::size_t n) override {
        fed.emplace_back(p, p + n);
    }
};
}  // namespace

TEST(AuQueue, PreservesAuBoundariesAndOrder) {
    AuQueue q(4096);
    auto a = au(0x10, 100), b = au(0x40, 250), c = au(0x80, 7);
    CHECK(q.push(a.data(), a.size()));
    CHECK(q.push(b.data(), b.size()));
    CHECK(q.push(c.data(), c.size()));
    CHECK_EQ(q.pushed(), std::uint64_t(3));

    std::vector<std::uint8_t> out;
    CHECK(q.pop(out)); CHECK(out == a);
    CHECK(q.pop(out)); CHECK(out == b);
    CHECK(q.pop(out)); CHECK(out == c);
    CHECK(!q.pop(out));            // empty
    CHECK_EQ(q.popped(), std::uint64_t(3));
    CHECK_EQ(q.dropped(), std::uint64_t(0));
}

TEST(AuQueue, DropsWholeAuWhenFull) {
    AuQueue q(200);  // rounds to 256 -> usable 255 bytes
    auto big = au(1, 200);        // 4 + 200 = 204 fits (51 free)
    CHECK(q.push(big.data(), big.size()));
    auto big2 = au(2, 200);       // 204 more won't fit in 51 -> dropped whole
    CHECK(!q.push(big2.data(), big2.size()));
    CHECK_EQ(q.dropped(), std::uint64_t(1));

    // The first AU is still intact and poppable (framing not corrupted by the drop).
    std::vector<std::uint8_t> out;
    CHECK(q.pop(out));
    CHECK(out == big);
}

TEST(AuQueue, WrapsAroundCleanly) {
    AuQueue q(256);
    std::vector<std::uint8_t> out;
    // Repeatedly push+pop an AU larger than half the buffer to force index wrap.
    for (int i = 0; i < 50; ++i) {
        auto a = au(std::uint8_t(i), 100);
        CHECK(q.push(a.data(), a.size()));
        CHECK(q.pop(out));
        CHECK(out == a);
    }
    CHECK_EQ(q.pending(), std::size_t(0));
}

TEST(AsyncH264Decoder, FeedEnqueuesPumpDecodes) {
    RecordingDecoder inner;
    AsyncH264Decoder dec(&inner, 8192);
    auto a = au(0x10, 64), b = au(0x20, 1000);

    dec.feed(a.data(), a.size());
    dec.feed(b.data(), b.size());
    // feed() must NOT have decoded anything yet — that's the whole point.
    CHECK_EQ(inner.fed.size(), std::size_t(0));

    const unsigned n = dec.pump(8);
    CHECK_EQ(n, 2u);
    REQUIRE(inner.fed.size() == 2);
    CHECK(inner.fed[0] == a);
    CHECK(inner.fed[1] == b);
}

TEST(AsyncH264Decoder, PumpRespectsMaxAus) {
    RecordingDecoder inner;
    AsyncH264Decoder dec(&inner, 8192);
    for (int i = 0; i < 5; ++i) { auto a = au(std::uint8_t(i), 10); dec.feed(a.data(), a.size()); }

    CHECK_EQ(dec.pump(2), 2u);             // only 2 this pass
    CHECK_EQ(inner.fed.size(), std::size_t(2));
    CHECK_EQ(dec.pump(8), 3u);             // remaining 3
    CHECK_EQ(inner.fed.size(), std::size_t(5));
    CHECK_EQ(dec.pump(8), 0u);             // drained
}

TEST(AsyncH264Decoder, DisabledDecodeStillDrains) {
    RecordingDecoder inner;
    AsyncH264Decoder dec(&inner, 8192);
    dec.setDecodeEnabled(false);           // the A/B "skip decode" instrumentation
    auto a = au(1, 50), b = au(2, 50);
    dec.feed(a.data(), a.size());
    dec.feed(b.data(), b.size());

    CHECK_EQ(dec.pump(8), 2u);             // popped both
    CHECK_EQ(inner.fed.size(), std::size_t(0));  // but decoded neither
    CHECK_EQ(dec.queue().popped(), std::uint64_t(2));
}

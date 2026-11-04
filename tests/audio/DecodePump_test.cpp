// DecodePump host tests with a scripted fake decoder: the scratch buffer always
// holds what decode() may write (whatever the decoder's channel count, even if
// it changes mid-stream), channels are mixed to the ring's count, boundaries are
// recorded only for real handoffs, and fill() stops at each boundary.
#include "audio/DecodePump.h"

#include <cstddef>
#include <vector>

#include "Test.h"
#include "audio/PcmRing.h"

using namespace carsh;

namespace {

// Emits `total` frames of `ch` channels; sample (f, c) = base + 0.01*c. Checks
// every decode() call against the pump's scratch capacity — an out-of-bounds
// write the real dr_* decoders would make silently (heap overflow) becomes a
// recorded violation here instead.
class FakeDecoder : public decode::IDecoder {
public:
    FakeDecoder(uint8_t ch, std::size_t total, float base)
        : ch_(ch), total_(total), base_(base) {}

    void watch(const audio::DecodePump* p) { pump_ = p; }
    // After `after` frames, start reporting (and writing) `ch` channels.
    void switchChannelsAt(std::size_t after, uint8_t ch) {
        switchAt_ = after;
        switchCh_ = ch;
    }

    bool open(hal::IStream*) override { return true; }
    std::size_t decode(float* out, std::size_t maxFrames) override {
        ++calls_;
        if (pump_ && maxFrames * ch_ > pump_->scratchSize()) overflow_ = true;
        std::size_t n = total_ - pos_ < maxFrames ? total_ - pos_ : maxFrames;
        for (std::size_t f = 0; f < n; ++f)
            for (uint8_t c = 0; c < ch_; ++c)
                out[f * ch_ + c] = base_ + 0.01f * float(c);
        pos_ += n;
        if (switchCh_ && pos_ >= switchAt_) { ch_ = switchCh_; switchCh_ = 0; }
        return n;
    }
    bool seek(uint64_t) override { return false; }
    decode::DecodedFormat format() const override {
        decode::DecodedFormat f;
        f.sampleRate = 48000;
        f.channels = ch_;
        f.totalFrames = total_;
        return f;
    }

    bool overflow() const { return overflow_; }
    int calls() const { return calls_; }

private:
    uint8_t ch_;
    std::size_t total_;
    float base_;
    std::size_t pos_ = 0;
    std::size_t switchAt_ = 0;
    uint8_t switchCh_ = 0;
    const audio::DecodePump* pump_ = nullptr;
    bool overflow_ = false;
    int calls_ = 0;
};

// Drains the ring into `out` (stereo frames).
std::size_t drain(audio::PcmRing<float>& ring, std::vector<float>& out) {
    float buf[512];
    std::size_t total = 0, got;
    while ((got = ring.read(buf, 512)) > 0) {
        out.insert(out.end(), buf, buf + got);
        total += got;
    }
    return total;
}

}  // namespace

TEST(DecodePump, SixChannelDecoderStaysInsideScratchAndIsDownmixed) {
    FakeDecoder d(6, 2000, 0.1f);
    audio::PcmRing<float> ring(8192);
    audio::DecodePump pump(&d, &ring, 2, 512);
    d.watch(&pump);
    std::vector<float> out;
    while (!pump.eof()) {
        pump.fill();
        drain(ring, out);
    }
    CHECK(!d.overflow());
    CHECK_EQ(pump.framesProduced(), std::size_t(2000));
    REQUIRE(out.size() == std::size_t(4000));  // stereo, one frame per input frame
    audio::ChannelMix ref;
    REQUIRE(ref.configure(6, 2));
    const float frame[6] = {0.10f, 0.11f, 0.12f, 0.13f, 0.14f, 0.15f};
    float want[2];
    ref.apply(frame, want, 1);
    CHECK_NEAR(out[0], want[0], 1e-6);
    CHECK_NEAR(out[1], want[1], 1e-6);
    CHECK_NEAR(out[3998], want[0], 1e-6);
    CHECK_NEAR(out[3999], want[1], 1e-6);
}

TEST(DecodePump, MonoDecoderIsUpmixedNotReinterpreted) {
    FakeDecoder d(1, 700, 0.5f);
    audio::PcmRing<float> ring(4096);
    audio::DecodePump pump(&d, &ring, 2, 256);
    d.watch(&pump);
    std::vector<float> out;
    while (!pump.eof()) {
        pump.fill();
        drain(ring, out);
    }
    CHECK(!d.overflow());
    REQUIRE(out.size() == std::size_t(1400));
    for (float s : out) CHECK_EQ(s, 0.5f);  // no stale-scratch half blocks
}

TEST(DecodePump, ChannelCountChangingMidStreamResizesScratch) {
    // A streaming decoder (e.g. ADTS AAC) may revise its channel count after the
    // first frames; the pump re-reads it before every decode().
    FakeDecoder d(1, 3000, 0.2f);
    d.switchChannelsAt(512, 6);
    audio::PcmRing<float> ring(16384);
    audio::DecodePump pump(&d, &ring, 2, 512);
    d.watch(&pump);
    std::vector<float> out;
    while (!pump.eof()) {
        pump.fill();
        drain(ring, out);
    }
    CHECK(!d.overflow());
    CHECK_EQ(pump.framesProduced(), std::size_t(3000));
}

TEST(DecodePump, UnmappableChannelCountIsTreatedAsDrained) {
    FakeDecoder bad(9, 1000, 0.3f);
    audio::PcmRing<float> ring(4096);
    audio::DecodePump pump(&bad, &ring, 2, 512);
    CHECK(!pump.fill());
    CHECK(pump.eof());
    CHECK_EQ(bad.calls(), 0);  // never handed a buffer it could overrun
    CHECK_EQ(ring.readable(), std::size_t(0));
}

TEST(DecodePump, BoundaryRecordedOnlyForRealHandoff) {
    FakeDecoder a(2, 300, 0.1f);
    audio::PcmRing<float> ring(4096);
    {
        audio::DecodePump pump(&a, &ring, 2, 128);
        while (pump.fill()) {}
        CHECK(pump.eof());
        CHECK_EQ(pump.tracksCompleted(), std::size_t(1));
        CHECK(pump.trackBoundaries().empty());  // no successor, no boundary
    }
    ring.reset();
    FakeDecoder b(2, 300, 0.1f), c(2, 200, 0.2f);
    audio::DecodePump pump(&b, &ring, 2, 128);
    pump.setNext(&c);
    while (!pump.eof()) pump.fill();
    REQUIRE(pump.trackBoundaries().size() == std::size_t(1));
    CHECK_EQ(pump.trackBoundaries()[0], std::size_t(300));
    CHECK_EQ(pump.tracksCompleted(), std::size_t(2));
}

TEST(DecodePump, FillStopsAtHandoffSoOwnerCanRotate) {
    FakeDecoder a(2, 100, 0.1f), b(2, 20000, 0.2f);
    audio::PcmRing<float> ring(8192);
    audio::DecodePump pump(&a, &ring, 2, 512);
    pump.setNext(&b);
    CHECK(pump.fill());
    // Returned at the seam: now on `b`, nothing of it decoded yet, room left.
    CHECK(pump.current() == &b);
    CHECK(!pump.hasNext());
    CHECK_EQ(pump.framesProduced(), std::size_t(100));
    CHECK(ring.writable() > 2);
    pump.fill();  // continues on b until the ring is full
    CHECK(ring.writable() < 2);
    CHECK(pump.current() == &b);
}

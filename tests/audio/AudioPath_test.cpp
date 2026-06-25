// End-to-end host audio path: IDecoder -> DecodePump -> PcmRing -> AudioEngine
// -> WavSinkDevice. Verifies passthrough fidelity, volume, underrun accounting,
// gapless handoff, and the soft limiter — the Phase-1 logic, provable by WAV.
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Test.h"
#include "audio/AudioEngine.h"
#include "audio/DecodePump.h"
#include "audio/PcmRing.h"
#include "audio/WavSinkDevice.h"
#include "decode/DecoderFactory.h"
#include "fs/HostVfs.h"

using namespace carsh;

static std::string fixture(const char* name) {
    return std::string(CARSH_FIXTURES_DIR) + "/" + name;
}

// Decode a fixture fully to interleaved float (reference signal).
static std::vector<float> refDecode(const std::string& path, uint8_t& ch,
                                    uint32_t& rate) {
    host::HostVfs vfs("/");
    auto s = vfs.open(path);
    auto d = decode::openAuto(s.get());
    auto f = d->format();
    ch = f.channels;
    rate = f.sampleRate;
    std::vector<float> out;
    float buf[2048];
    std::size_t got;
    while ((got = d->decode(buf, 1024)) > 0)
        out.insert(out.end(), buf, buf + got * ch);
    return out;
}

TEST(AudioPath, WavPassthroughIsBitExact) {
    uint8_t ch; uint32_t rate;
    std::vector<float> ref = refDecode(fixture("sine.wav"), ch, rate);
    REQUIRE(ref.size() > 1000);

    host::HostVfs vfs("/");
    auto stream = vfs.open(fixture("sine.wav"));
    auto dec = decode::openAuto(stream.get());
    REQUIRE(dec != nullptr);

    audio::PcmRing<float> ring(size_t(rate) * ch);  // ~1s
    audio::AudioEngine eng(&ring, ch);
    host::WavSinkDevice dev("/tmp/carsh_path.wav");
    REQUIRE(dev.open({rate, 16, ch}, &eng));
    audio::DecodePump pump(dec.get(), &ring, ch, 512);

    // Drive: keep ring fed, pump device in blocks, until pump EOF + ring empty.
    pump.fill();
    while (!pump.eof() || ring.readable() > 0) {
        dev.pump(256);
        pump.fill();
    }

    const auto& cap = eng.channels() ? dev.captured() : dev.captured();
    // Captured may have a trailing partial/zero block; compare the ref prefix.
    REQUIRE(cap.size() >= ref.size());
    double maxErr = 0;
    for (std::size_t i = 0; i < ref.size(); ++i)
        maxErr = std::fmax(maxErr, std::fabs(cap[i] - ref[i]));
    CHECK(maxErr < 1e-6);  // volume 1.0, no DSP -> identical floats
    // Tail beyond ref must be zero-fill (underrun region).
    for (std::size_t i = ref.size(); i < cap.size(); ++i)
        CHECK(std::fabs(cap[i]) < 1e-6);
}

TEST(AudioPath, MasterVolumeScales) {
    uint8_t ch; uint32_t rate;
    std::vector<float> ref = refDecode(fixture("sine.wav"), ch, rate);
    host::HostVfs vfs("/");
    auto stream = vfs.open(fixture("sine.wav"));
    auto dec = decode::openAuto(stream.get());

    audio::PcmRing<float> ring(size_t(rate) * ch);
    audio::AudioEngine eng(&ring, ch);
    eng.setMasterVolume(0.5f);
    host::WavSinkDevice dev("/tmp/carsh_vol.wav");
    dev.open({rate, 16, ch}, &eng);
    audio::DecodePump pump(dec.get(), &ring, ch, 512);
    pump.fill();
    while (!pump.eof() || ring.readable() > 0) { dev.pump(256); pump.fill(); }

    const auto& cap = dev.captured();
    double maxErr = 0;
    for (std::size_t i = 0; i < ref.size(); ++i)
        maxErr = std::fmax(maxErr, std::fabs(cap[i] - 0.5f * ref[i]));
    CHECK(maxErr < 1e-6);
}

TEST(AudioPath, UnderrunCountedAndZeroFilled) {
    // Open device but never fill the ring; every pulled frame underruns.
    audio::PcmRing<float> ring(1024);
    audio::AudioEngine eng(&ring, 2);
    host::WavSinkDevice dev("/tmp/carsh_under.wav");
    dev.open({48000, 16, 2}, &eng);
    dev.pump(500);
    CHECK_EQ(eng.underrunFrames(), std::size_t(500));
    // All captured samples are silence.
    for (float s : dev.captured()) CHECK(std::fabs(s) < 1e-9);
}

TEST(AudioPath, GaplessHandoffHasNoGapAndDoublesLength) {
    uint8_t ch; uint32_t rate;
    std::vector<float> ref = refDecode(fixture("sine.wav"), ch, rate);
    host::HostVfs vfs("/");
    auto s1 = vfs.open(fixture("sine.wav"));
    auto s2 = vfs.open(fixture("sine.wav"));
    auto d1 = decode::openAuto(s1.get());
    auto d2 = decode::openAuto(s2.get());

    audio::PcmRing<float> ring(size_t(rate) * ch / 4);  // smaller ring forces refills
    audio::AudioEngine eng(&ring, ch);
    host::WavSinkDevice dev("/tmp/carsh_gapless.wav");
    dev.open({rate, 16, ch}, &eng);
    audio::DecodePump pump(d1.get(), &ring, ch, 256);
    pump.setNext(d2.get());  // gapless second track

    pump.fill();
    while (!pump.eof() || ring.readable() > 0) { dev.pump(256); pump.fill(); }

    CHECK_EQ(pump.framesProduced(), std::size_t(ref.size() / ch) * 2);
    // Verify continuity across the seam: the concatenation equals ref+ref over
    // the first 2*ref samples (no inserted silence at the boundary).
    const auto& cap = dev.captured();
    REQUIRE(cap.size() >= ref.size() * 2);
    double maxErr = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        maxErr = std::fmax(maxErr, std::fabs(cap[i] - ref[i]));
        maxErr = std::fmax(maxErr, std::fabs(cap[ref.size() + i] - ref[i]));
    }
    CHECK(maxErr < 1e-6);
}

TEST(AudioPath, SoftLimiterBoundsOutput) {
    audio::PcmRing<float> ring(1024);
    audio::AudioEngine eng(&ring, 1);
    eng.setReplayGainLinear(4.0f);  // +12 dB boost -> would clip hard
    eng.setSoftLimiter(true);
    host::WavSinkDevice dev("/tmp/carsh_lim.wav");
    dev.open({48000, 16, 1}, &eng);

    std::vector<float> loud(256);
    for (int i = 0; i < 256; ++i) loud[i] = (i % 2) ? 0.6f : -0.6f;  // *4 = ±2.4
    ring.write(loud.data(), loud.size());
    dev.pump(256);

    for (float s : dev.captured()) CHECK(std::fabs(s) <= 1.0f);  // bounded to FS
    // And a small input passes through near-unaffected (identity below knee).
    eng.setReplayGainLinear(1.0f);
    audio::PcmRing<float> ring2(1024);
    audio::AudioEngine eng2(&ring2, 1);
    eng2.setSoftLimiter(true);
    host::WavSinkDevice dev2("/tmp/carsh_lim2.wav");
    dev2.open({48000, 16, 1}, &eng2);
    float quiet[4] = {0.1f, -0.2f, 0.3f, -0.05f};
    ring2.write(quiet, 4);
    dev2.pump(4);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(dev2.captured()[i], quiet[i], 1e-6);
}

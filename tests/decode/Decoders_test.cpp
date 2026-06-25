#include "decode/DecoderFactory.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Test.h"
#include "fs/HostVfs.h"
#include "fs/MemStream.h"

using namespace carsh;
namespace fs = carsh::host;

static std::string fixture(const char* name) {
    return std::string(CARSH_FIXTURES_DIR) + "/" + name;
}

// Decode an entire file to interleaved float via the auto-factory.
static bool decodeAll(const std::string& path, std::vector<float>& out,
                      decode::DecodedFormat& fmt) {
    fs::HostVfs vfs("/");  // absolute fixture paths
    auto stream = vfs.open(path);
    if (!stream) return false;
    auto dec = decode::openAuto(stream.get());
    if (!dec) return false;
    fmt = dec->format();
    float buf[4096];
    for (;;) {
        std::size_t got = dec->decode(buf, 1024);  // small blocks (yield model)
        if (got == 0) break;
        out.insert(out.end(), buf, buf + got * fmt.channels);
    }
    return true;
}

TEST(Decode, SniffIdentifiesFormatsFromMagic) {
    // Craft minimal headers; sniff must not need a full parse.
    std::vector<uint8_t> wav = {'R','I','F','F',0,0,0,0,'W','A','V','E'};
    std::vector<uint8_t> flac = {'f','L','a','C',0,0,0,0,0,0,0,0};
    std::vector<uint8_t> id3 = {'I','D','3',3,0,0,0,0,0,0,0,0};
    std::vector<uint8_t> mpeg = {0xFF,0xFB,0x90,0x00,0,0,0,0,0,0,0,0};
    fs::MemStream sw(wav), sf(flac), s3(id3), sm(mpeg);
    CHECK(decode::sniff(&sw) == decode::Codec::Wav);
    CHECK(decode::sniff(&sf) == decode::Codec::Flac);
    CHECK(decode::sniff(&s3) == decode::Codec::Mp3);
    CHECK(decode::sniff(&sm) == decode::Codec::Mp3);
}

TEST(Decode, WavMatchesKnownSine) {
    std::vector<float> pcm;
    decode::DecodedFormat fmt;
    REQUIRE(decodeAll(fixture("sine.wav"), pcm, fmt));
    CHECK_EQ(fmt.sampleRate, 44100u);
    CHECK_EQ(fmt.channels, uint8_t(2));
    REQUIRE(pcm.size() > 2000);
    // Reconstruct expected 16-bit-quantized sine and compare. L=440, R=660.
    double maxErr = 0;
    std::size_t frames = pcm.size() / 2;
    for (std::size_t i = 0; i < frames; ++i) {
        double t = double(i) / 44100.0;
        double l = std::round(0.5 * std::sin(2 * M_PI * 440 * t) * 32767) / 32768.0;
        double r = std::round(0.5 * std::sin(2 * M_PI * 660 * t) * 32767) / 32768.0;
        maxErr = std::fmax(maxErr, std::fabs(pcm[i * 2] - l));
        maxErr = std::fmax(maxErr, std::fabs(pcm[i * 2 + 1] - r));
    }
    CHECK(maxErr < 1.0 / 30000.0);
}

TEST(Decode, FlacIsLosslessVsWav) {
    std::string flacPath = fixture("sine.flac");
    fs::HostVfs vfs("/");
    auto probe = vfs.open(flacPath);
    if (!probe) {
        std::printf("  (skip) no FLAC fixture — encoder unavailable\n");
        return;
    }
    std::vector<float> wavPcm, flacPcm;
    decode::DecodedFormat wf, ff;
    REQUIRE(decodeAll(fixture("sine.wav"), wavPcm, wf));
    REQUIRE(decodeAll(flacPath, flacPcm, ff));
    CHECK_EQ(ff.sampleRate, wf.sampleRate);
    CHECK_EQ(ff.channels, wf.channels);
    REQUIRE(flacPcm.size() == wavPcm.size());
    // FLAC of a 16-bit WAV is bit-exact; float reconstruction must be identical.
    double maxErr = 0;
    for (std::size_t i = 0; i < wavPcm.size(); ++i)
        maxErr = std::fmax(maxErr, std::fabs(wavPcm[i] - flacPcm[i]));
    CHECK(maxErr < 1e-6);
}

TEST(Decode, Mp3DecodesToCorrelatedSignal) {
    std::string mp3Path = fixture("sine.mp3");
    fs::HostVfs vfs("/");
    auto probe = vfs.open(mp3Path);
    if (!probe) {
        std::printf("  (skip) no MP3 fixture — encoder unavailable\n");
        return;
    }
    std::vector<float> pcm;
    decode::DecodedFormat fmt;
    REQUIRE(decodeAll(mp3Path, pcm, fmt));
    CHECK_EQ(fmt.sampleRate, 44100u);
    CHECK_EQ(fmt.channels, uint8_t(2));
    REQUIRE(pcm.size() > 8000);
    // MP3 is lossy + has encoder/decoder delay. Verify the left channel is a
    // ~440 Hz tone via the Goertzel magnitude over a stable middle window,
    // dominating an off-tone reference bin, and that it isn't silent.
    std::size_t frames = pcm.size() / 2;
    std::size_t start = 2000, end = frames - 2000;
    auto goertzel = [&](double freq) {
        double w = 2 * M_PI * freq / 44100.0;
        double coeff = 2 * std::cos(w), s0 = 0, s1 = 0, s2 = 0;
        for (std::size_t i = start; i < end; ++i) {
            s0 = pcm[i * 2] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
    };
    double onTone = goertzel(440.0);
    double offTone = goertzel(440.0 * 3);  // 1320 Hz, should be far weaker
    CHECK(onTone > 1.0);
    CHECK(onTone > offTone * 5.0);
}

TEST(Decode, SeekRewindsWav) {
    fs::HostVfs vfs("/");
    auto stream = vfs.open(fixture("sine.wav"));
    REQUIRE(stream != nullptr);
    auto dec = decode::openAuto(stream.get());
    REQUIRE(dec != nullptr);
    float a[512 * 2], b[512 * 2];
    REQUIRE(dec->decode(a, 512) == 512);
    REQUIRE(dec->seek(0));
    REQUIRE(dec->decode(b, 512) == 512);
    for (int i = 0; i < 512 * 2; ++i) CHECK_NEAR(a[i], b[i], 1e-9);
}

TEST(Decode, CodecFromExtension) {
    CHECK(decode::codecFromExtension("/x/song.FLAC") == decode::Codec::Flac);
    CHECK(decode::codecFromExtension("song.mp3") == decode::Codec::Mp3);
    CHECK(decode::codecFromExtension("a.b.wav") == decode::Codec::Wav);
    CHECK(decode::codecFromExtension("noext") == decode::Codec::Unknown);
}

// AAC (ADTS) decode test. Skips gracefully without the fixture/faad2.
#include <cmath>
#include <string>
#include <vector>

#include "Test.h"
#include "decode/DecoderFactory.h"
#include "fs/HostVfs.h"

using namespace carsh;

static std::string fixture(const char* n) {
    return std::string(CARSH_FIXTURES_DIR) + "/" + n;
}

TEST(Aac, DecodesToCorrelatedTone) {
    host::HostVfs vfs("/");
    auto stream = vfs.open(fixture("sine.aac"));
    if (!stream) {
        std::printf("  (skip) no AAC fixture — faac unavailable\n");
        return;
    }
    CHECK(decode::sniff(stream.get()) == decode::Codec::Aac);  // not misread as MP3
    auto dec = decode::openAuto(stream.get());
    if (!dec) {
        std::printf("  (skip) AAC decoder not compiled (no faad2)\n");
        return;
    }
    auto fmt = dec->format();
    CHECK_EQ(fmt.sampleRate, 44100u);
    CHECK(fmt.channels >= 1);

    std::vector<float> pcm;
    float buf[4096];
    std::size_t got;
    while ((got = dec->decode(buf, 777)) > 0)  // odd block to exercise pending_
        pcm.insert(pcm.end(), buf, buf + got * fmt.channels);
    REQUIRE(pcm.size() > 10000);

    std::size_t frames = pcm.size() / fmt.channels;
    std::size_t start = 3000, end = frames - 3000;
    auto goertzel = [&](double freq) {
        double w = 2 * M_PI * freq / fmt.sampleRate;
        double coeff = 2 * std::cos(w), s1 = 0, s2 = 0;
        for (std::size_t i = start; i < end; ++i) {
            double s0 = pcm[i * fmt.channels] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
    };
    CHECK(goertzel(440.0) > goertzel(440.0 * 3) * 4.0);
}

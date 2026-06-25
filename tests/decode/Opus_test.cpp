// Opus decode test. Skips gracefully when the fixture/library is absent so the
// suite stays green on a build server without libopusfile.
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

TEST(Opus, DecodesToCorrelatedTone) {
    host::HostVfs vfs("/");
    auto stream = vfs.open(fixture("sine.opus"));
    if (!stream) {
        std::printf("  (skip) no Opus fixture — opusenc unavailable\n");
        return;
    }
    auto dec = decode::openAuto(stream.get());
    if (!dec) {
        std::printf("  (skip) Opus decoder not compiled (no libopusfile)\n");
        return;
    }
    auto fmt = dec->format();
    CHECK_EQ(fmt.sampleRate, 48000u);  // Opus always resamples to 48 kHz
    CHECK(fmt.channels >= 1);

    std::vector<float> pcm;
    float buf[2048];
    std::size_t got;
    while ((got = dec->decode(buf, 1024)) > 0)
        pcm.insert(pcm.end(), buf, buf + got * fmt.channels);
    REQUIRE(pcm.size() > 10000);

    // Left channel should carry a ~440 Hz tone (resampled from 44.1k source).
    std::size_t frames = pcm.size() / fmt.channels;
    std::size_t start = 4000, end = frames - 4000;
    auto goertzel = [&](double freq) {
        double w = 2 * M_PI * freq / 48000.0;
        double coeff = 2 * std::cos(w), s1 = 0, s2 = 0;
        for (std::size_t i = start; i < end; ++i) {
            double s0 = pcm[i * fmt.channels] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
    };
    CHECK(goertzel(440.0) > goertzel(440.0 * 3) * 4.0);
}

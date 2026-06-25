#include "dsp/Loudness.h"

#include <cmath>
#include <vector>

#include "Test.h"
#include "dsp/ReplayGain.h"

using namespace carsh::dsp;

// Generate `sec` seconds of a stereo sine at amplitude A and measure loudness.
static double sineLufs(double amp, double freq = 1000, double sec = 3.0,
                       double fs = 48000) {
    LoudnessMeter m(fs, 2);
    std::size_t n = std::size_t(fs * sec);
    std::vector<float> buf(1024 * 2);
    std::size_t done = 0;
    while (done < n) {
        std::size_t block = std::min<std::size_t>(1024, n - done);
        for (std::size_t i = 0; i < block; ++i) {
            float s = float(amp * std::sin(2 * M_PI * freq * double(done + i) / fs));
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        m.process(buf.data(), block);
        done += block;
    }
    return m.integratedLufs();
}

TEST(Loudness, HalvingAmplitudeDrops6dB) {
    double loud = sineLufs(0.5);
    double quiet = sineLufs(0.25);
    CHECK_NEAR(loud - quiet, 6.02, 0.2);
}

TEST(Loudness, KWeightingShape) {
    LoudnessMeter m(48000, 2);
    // High-shelf gives ~+4 dB well above the shelf corner.
    double hi = 20.0 * std::log10(m.kMagnitudeAt(10000, 48000));
    CHECK_NEAR(hi, 4.0, 0.7);
    // RLB high-pass strongly attenuates sub-bass.
    double lo = 20.0 * std::log10(m.kMagnitudeAt(10, 48000));
    CHECK(lo < -18.0);  // ~-23 dB at 10 Hz (2nd-order RLB high-pass)
    // Near unity in the 1–2 kHz reference region.
    double mid = 20.0 * std::log10(m.kMagnitudeAt(1000, 48000));
    CHECK(std::fabs(mid) < 2.0);
}

TEST(Loudness, SilenceGatedOut) {
    // 1 s loud + 3 s silence: the silent blocks fall below the -70 abs gate and
    // the relative gate, so the integrated value reflects the loud section only.
    const double fs = 48000;
    LoudnessMeter m(fs, 2);
    std::vector<float> buf(1024 * 2);
    auto feed = [&](double amp, double sec) {
        std::size_t n = std::size_t(fs * sec), done = 0;
        static std::size_t phase = 0;
        while (done < n) {
            std::size_t b = std::min<std::size_t>(1024, n - done);
            for (std::size_t i = 0; i < b; ++i) {
                float s = float(amp * std::sin(2 * M_PI * 1000 * double(phase) / fs));
                buf[i * 2] = s; buf[i * 2 + 1] = s; ++phase;
            }
            m.process(buf.data(), b);
            done += b;
        }
    };
    feed(0.5, 1.0);
    feed(0.0, 3.0);
    double gated = m.integratedLufs();
    double loudOnly = sineLufs(0.5, 1000, 3.0);
    CHECK_NEAR(gated, loudOnly, 1.0);  // silence didn't drag it down
}

TEST(Loudness, PeakTracked) {
    LoudnessMeter m(48000, 1);
    std::vector<float> buf = {0.1f, -0.7f, 0.3f, -0.2f};
    m.process(buf.data(), buf.size());
    CHECK_NEAR(m.samplePeak(), 0.7f, 1e-6);
}

TEST(Loudness, GainToReferenceNormalizes) {
    double measured = sineLufs(0.5);
    double gainDb = gainToReference(measured);
    // Applying the gain should bring a re-measured signal close to -23 LUFS.
    double normalized = sineLufs(0.5 * dbToLinear(gainDb));
    CHECK_NEAR(normalized, kReferenceLufs, 0.3);
}

#include <cmath>
#include <vector>

#include "Test.h"
#include "dsp/Biquad.h"
#include "dsp/Eq.h"
#include "dsp/ReplayGain.h"

using namespace carsh::dsp;

static double dbLin(double db) { return std::pow(10.0, db / 20.0); }

// --- Biquad coefficient / analytic-response checks --------------------------

TEST(Biquad, PeakingHitsGainAtCenter) {
    const double fs = 48000, f0 = 1000, g = 6.0, q = 1.0;
    auto c = BiquadCoeffs::design(FilterType::Peaking, f0, fs, g, q);
    // At f0 the peaking filter's magnitude equals the linear gain.
    CHECK_NEAR(c.magnitudeAt(f0, fs), dbLin(g), 1e-3);
    // Far from f0 it is ~unity (no effect).
    CHECK_NEAR(c.magnitudeAt(50, fs), 1.0, 0.02);
    CHECK_NEAR(c.magnitudeAt(20000, fs), 1.0, 0.05);
}

TEST(Biquad, PeakingCutIsReciprocal) {
    const double fs = 48000, f0 = 2000;
    auto boost = BiquadCoeffs::design(FilterType::Peaking, f0, fs, 9.0, 1.4);
    auto cut = BiquadCoeffs::design(FilterType::Peaking, f0, fs, -9.0, 1.4);
    CHECK_NEAR(boost.magnitudeAt(f0, fs) * cut.magnitudeAt(f0, fs), 1.0, 1e-3);
}

TEST(Biquad, LowPassPassesLowBlocksHigh) {
    const double fs = 48000, fc = 1000;
    auto c = BiquadCoeffs::design(FilterType::LowPass, fc, fs, 0, 0.707);
    CHECK_NEAR(c.magnitudeAt(50, fs), 1.0, 0.02);        // passband ~unity
    CHECK_NEAR(c.magnitudeAt(fc, fs), dbLin(-3.0), 0.05);  // ~-3 dB at cutoff
    CHECK(c.magnitudeAt(12000, fs) < 0.05);              // stopband attenuated
}

TEST(Biquad, HighPassBlocksLowPassesHigh) {
    const double fs = 48000, fc = 1000;
    auto c = BiquadCoeffs::design(FilterType::HighPass, fc, fs, 0, 0.707);
    CHECK(c.magnitudeAt(50, fs) < 0.05);
    CHECK_NEAR(c.magnitudeAt(fc, fs), dbLin(-3.0), 0.05);
    CHECK_NEAR(c.magnitudeAt(20000, fs), 1.0, 0.05);
}

TEST(Biquad, ShelvesBoostCorrectBand) {
    const double fs = 48000;
    auto low = BiquadCoeffs::design(FilterType::LowShelf, 200, fs, 6, 0.707);
    CHECK_NEAR(low.magnitudeAt(20, fs), dbLin(6), 0.1);    // low end boosted
    CHECK_NEAR(low.magnitudeAt(20000, fs), 1.0, 0.05);     // high end unaffected
    auto high = BiquadCoeffs::design(FilterType::HighShelf, 5000, fs, -6, 0.707);
    CHECK_NEAR(high.magnitudeAt(20000, fs), dbLin(-6), 0.1);
    CHECK_NEAR(high.magnitudeAt(50, fs), 1.0, 0.05);
}

// --- EQ processing: measured gain matches the analytic response -------------

static double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(to - from));
}

TEST(Eq, ProcessedAmplitudeMatchesResponse) {
    const double fs = 48000, f0 = 1000;
    EqProcessor eq(fs);
    EqConfig cfg;
    cfg.bands.push_back({FilterType::Peaking, f0, 6.0, 1.0});
    eq.setConfig(cfg);

    const int n = 48000;  // 1 s mono
    std::vector<float> sig(n);
    for (int i = 0; i < n; ++i)
        sig[i] = float(0.4 * std::sin(2 * M_PI * f0 * i / fs));
    std::vector<float> in = sig;
    eq.process(sig.data(), n, 1);

    // Steady-state ratio over the latter half (skip the filter transient).
    double ratio = rms(sig, n / 2, n) / rms(in, n / 2, n);
    CHECK_NEAR(ratio, eq.magnitudeAt(f0), 0.02);
    CHECK_NEAR(ratio, dbLin(6.0), 0.03);
}

TEST(Eq, FlatConfigIsPassthrough) {
    EqProcessor eq(48000);
    EqConfig cfg;  // no bands, 0 preamp
    eq.setConfig(cfg);
    std::vector<float> x = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f};
    std::vector<float> orig = x;
    eq.process(x.data(), x.size(), 1);
    for (std::size_t i = 0; i < x.size(); ++i) CHECK_NEAR(x[i], orig[i], 1e-6);
}

TEST(Eq, PreampScales) {
    EqProcessor eq(48000);
    EqConfig cfg;
    cfg.preampDb = -6.0;  // halve-ish
    eq.setConfig(cfg);
    std::vector<float> x = {1.0f, -1.0f, 0.5f};
    eq.process(x.data(), x.size(), 1);
    CHECK_NEAR(x[0], dbLin(-6.0), 1e-4);
    CHECK_NEAR(x[1], -dbLin(-6.0), 1e-4);
}

TEST(Eq, CoefficientSwapStaysStable) {
    EqProcessor eq(48000);
    EqConfig a;
    a.bands.push_back({FilterType::Peaking, 1000, 12.0, 2.0});
    EqConfig b;
    b.bands.push_back({FilterType::HighShelf, 6000, -10.0, 0.707});
    b.bands.push_back({FilterType::LowShelf, 120, 8.0, 0.707});

    std::vector<float> buf(1024);
    for (int i = 0; i < 1024; ++i) buf[i] = float(0.3 * std::sin(0.05 * i));
    eq.setConfig(a);
    eq.process(buf.data(), 512, 1);
    eq.setConfig(b);  // swap mid-stream; state persists
    eq.process(buf.data() + 512, 512, 1);
    for (float v : buf) {
        CHECK(std::isfinite(v));
        CHECK(std::fabs(v) < 10.0f);  // bounded, no blow-up
    }
}

// --- ReplayGain -------------------------------------------------------------

TEST(ReplayGain, DbLinearRoundTrip) {
    for (double db : {-12.0, -6.0, 0.0, 3.0, 9.0}) {
        float lin = dbToLinear(db);
        CHECK_NEAR(linearToDb(lin), db, 1e-4);
    }
}

TEST(ReplayGain, GainToReferenceMatchesR128) {
    // A track measured at -16 LUFS is 7 dB too loud vs -23 -> gain -7 dB.
    CHECK_NEAR(gainToReference(-16.0), -7.0, 1e-9);
    CHECK_NEAR(gainToReference(-30.0), 7.0, 1e-9);
}

TEST(ReplayGain, ResolveModesAndPeakClamp) {
    RgInfo info;
    info.trackGainDb = -7.0f; info.albumGainDb = -5.0f;
    info.trackPeak = 0.9f;    info.albumPeak = 0.95f;
    CHECK_NEAR(resolveGain(info, RgMode::Track), dbToLinear(-7.0), 1e-4);
    CHECK_NEAR(resolveGain(info, RgMode::Album), dbToLinear(-5.0), 1e-4);
    CHECK_EQ(resolveGain(info, RgMode::Off), 1.0f);

    // Positive gain that would clip is clamped so gain*peak == ceiling.
    RgInfo hot;
    hot.trackGainDb = 12.0f;  // *3.98
    hot.trackPeak = 0.8f;     // 3.98*0.8 = 3.18 >> 1
    float g = resolveGain(hot, RgMode::Track);
    CHECK_NEAR(g * hot.trackPeak, 1.0, 1e-4);
}

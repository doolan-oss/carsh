// GraphicEq — the default EQ band layout the device seeds into the EQ tab and the
// engine EQ (review P5: the EQ tab was blank because nothing ever gave it bands).
#include "dsp/GraphicEq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Test.h"
#include "dsp/Eq.h"
#include "ui/Screens.h"

using namespace carsh;

TEST(GraphicEq, DefaultIsTenFlatOctaveBands) {
    const dsp::EqConfig cfg = dsp::defaultGraphicEq();
    REQUIRE(cfg.bands.size() == 10u);
    CHECK(cfg.bands.size() <= std::size_t(dsp::EqProcessor::kMaxBands));
    CHECK_EQ(cfg.preampDb, 0.0);
    double prev = 0.0;
    for (const dsp::EqBand& b : cfg.bands) {
        CHECK(b.type == dsp::FilterType::Peaking);
        CHECK_EQ(b.gainDb, 0.0);
        CHECK(b.q > 1.0 && b.q < 2.0);
        CHECK(b.freqHz > prev);             // ascending, one per octave
        if (prev > 0) CHECK(std::fabs(b.freqHz / prev - 2.0) < 0.05);
        CHECK(b.freqHz < 44100.0 / 2.0);    // below Nyquist at 44.1 kHz and up
        prev = b.freqHz;
    }
    CHECK_EQ(cfg.bands.front().freqHz, 31.5);
    CHECK_EQ(cfg.bands.back().freqHz, 16000.0);
}

// Flat means transparent: the engine EQ configured with the default passes audio
// through unchanged (so seeding it at boot changes nothing until the user edits).
TEST(GraphicEq, DefaultIsTransparentInTheEngine) {
    dsp::EqProcessor eq(48000);
    eq.setConfig(dsp::defaultGraphicEq());
    for (double f : {40.0, 440.0, 1000.0, 5000.0, 15000.0})
        CHECK(std::fabs(eq.magnitudeAt(f) - 1.0) < 1e-6);
    float buf[2 * 256];
    for (int i = 0; i < 256; ++i) {
        const float s = float(std::sin(2.0 * M_PI * 1000.0 * i / 48000.0)) * 0.5f;
        buf[2 * i] = s;
        buf[2 * i + 1] = -s;
    }
    float orig[2 * 256];
    for (int i = 0; i < 512; ++i) orig[i] = buf[i];
    eq.process(buf, 256, 2);
    float maxErr = 0.0f;
    for (int i = 0; i < 512; ++i) maxErr = std::max(maxErr, std::fabs(buf[i] - orig[i]));
    CHECK(maxErr < 1e-4f);
}

TEST(GraphicEq, ProfileCurveWinsWhenStored) {
    // No stored curve: the default layout.
    CHECK_EQ(dsp::eqForProfile(dsp::EqConfig{}).bands.size(), std::size_t(10));
    // A stored curve is used as-is.
    dsp::EqConfig mine;
    mine.preampDb = -3.0;
    mine.bands = {{dsp::FilterType::Peaking, 100, 4.0, 1.0},
                  {dsp::FilterType::HighShelf, 8000, -2.0, 0.7}};
    const dsp::EqConfig got = dsp::eqForProfile(mine);
    REQUIRE(got.bands.size() == 2u);
    CHECK_EQ(got.preampDb, -3.0);
    CHECK_EQ(got.bands[0].gainDb, 4.0);
    CHECK(got.bands[1].type == dsp::FilterType::HighShelf);
    // More bands than the DSP runs: clamped, so the screen matches what you hear.
    dsp::EqConfig big;
    for (int i = 0; i < 20; ++i)
        big.bands.push_back({dsp::FilterType::Peaking, 50.0 * (i + 1), 0.0, 1.0});
    CHECK_EQ(dsp::eqForProfile(big).bands.size(),
             std::size_t(dsp::EqProcessor::kMaxBands));
}

// The seeded layout makes the EQ tab usable: a tap on a band column boosts it.
TEST(GraphicEq, SeededEqScreenTakesTaps) {
    const ui::Rect c{52, 0, 748, 432};  // the device content rect
    ui::EqScreen screen(c);
    ui::Gesture tap;
    tap.type = ui::GestureType::Tap;
    tap.x = c.x + 20 + 5;               // first column
    tap.y = c.y + 20 + 10;              // upper half -> boost
    CHECK(!screen.handle(tap));         // unseeded: blank, taps ignored (the bug)

    screen.setConfig(dsp::defaultGraphicEq());
    int band = -1;
    double gain = 0.0;
    screen.onBandChange = [&](int b, double g) { band = b; gain = g; };
    // The last (16 kHz) column is reachable too.
    const int areaW = c.w - 40, colW = areaW / 10;
    CHECK(screen.handle(tap));
    CHECK_EQ(band, 0);
    CHECK_EQ(gain, 1.0);
    tap.x = c.x + 20 + 9 * colW + colW / 2;
    CHECK(screen.handle(tap));
    CHECK_EQ(band, 9);
}

// --- The EQ follows the output rate (final review) -----------------------------
// The engine EQ used to be designed at a fixed 48 kHz while the jack ran at the
// track's rate, so on 44.1 kHz material every edited band sat 8.1 % low.

namespace {
// Steady-state gain (dB) of `eq` for a sine at freqHz sampled at sampleRate.
double measuredGainDb(dsp::EqProcessor& eq, double freqHz, double sampleRate) {
    const int n = int(sampleRate);  // 1 s, mono
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[std::size_t(i)] = float(0.25 * std::sin(2.0 * M_PI * freqHz * i / sampleRate));
    std::vector<float> y = x;
    eq.reset();
    eq.process(y.data(), y.size(), 1);
    double ex = 0, ey = 0;
    for (int i = n / 2; i < n; ++i) {  // skip the filter's transient
        ex += double(x[std::size_t(i)]) * x[std::size_t(i)];
        ey += double(y[std::size_t(i)]) * y[std::size_t(i)];
    }
    return 10.0 * std::log10(ey / ex);
}
dsp::EqConfig narrowBoostAt1k() {
    dsp::EqConfig cfg;
    cfg.bands.push_back({dsp::FilterType::Peaking, 1000.0, 6.0, 4.0});  // Q 4: ~250 Hz
    return cfg;
}
}  // namespace

TEST(GraphicEq, BandsActAtTheirFrequencyAt44k1Too) {
    // Designed for 48 kHz, fed 44.1 kHz audio (the old device behaviour): the band
    // acts at ~919 Hz, so a 1 kHz tone gets well under the +6 dB asked for...
    dsp::EqProcessor stale(48000);
    stale.setConfig(narrowBoostAt1k());
    CHECK(measuredGainDb(stale, 1000.0, 44100.0) < 4.5);
    CHECK_NEAR(measuredGainDb(stale, 1000.0 * 44100.0 / 48000.0, 44100.0), 6.0, 0.15);

    // ...re-designed for the rate the output runs at, it lands on 1 kHz.
    dsp::EqProcessor eq(48000);
    eq.setConfig(narrowBoostAt1k());
    eq.setSampleRate(44100);
    CHECK_EQ(eq.sampleRate(), 44100.0);
    CHECK_NEAR(measuredGainDb(eq, 1000.0, 44100.0), 6.0, 0.15);
    CHECK_NEAR(eq.magnitudeAt(1000.0), std::pow(10.0, 6.0 / 20.0), 1e-3);
    // And back: at 48 kHz it is the original design again.
    eq.setSampleRate(48000);
    CHECK_NEAR(measuredGainDb(eq, 1000.0, 48000.0), 6.0, 0.15);
    // The curve survives a rate change: a later edit keeps the new rate.
    eq.setSampleRate(44100);
    dsp::EqConfig cut = narrowBoostAt1k();
    cut.bands[0].gainDb = -6.0;
    eq.setConfig(cut);
    CHECK_NEAR(measuredGainDb(eq, 1000.0, 44100.0), -6.0, 0.15);
}

TEST(GraphicEq, SampleRateBeforeConfigOnlyRecordsIt) {
    dsp::EqProcessor eq(48000);
    eq.setSampleRate(44100);  // not configured yet: nothing is designed or run
    float x[4] = {0.5f, -0.5f, 0.25f, -0.25f};
    eq.process(x, 4, 1);
    CHECK_EQ(x[0], 0.5f);
    eq.setConfig(narrowBoostAt1k());  // designed at the recorded rate
    CHECK_NEAR(measuredGainDb(eq, 1000.0, 44100.0), 6.0, 0.15);
    eq.setSampleRate(0);              // ignored
    CHECK_EQ(eq.sampleRate(), 44100.0);
}

// A rate change mid-stream is the same bank swap as an edit: no blow-up.
TEST(GraphicEq, RateChangeMidStreamStaysStable) {
    dsp::EqProcessor eq(44100);
    dsp::EqConfig cfg = dsp::defaultGraphicEq();
    for (dsp::EqBand& b : cfg.bands) b.gainDb = 12.0;  // everything boosted
    eq.setConfig(cfg);
    std::vector<float> buf(2 * 4096);
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = float(0.05 * std::sin(0.03 * double(i)));
    const std::size_t block = 512;
    const double rates[] = {48000, 44100, 96000, 22050, 32000, 44100};
    for (std::size_t k = 0; k * block * 2 < buf.size(); ++k) {
        eq.setSampleRate(rates[k % 6]);
        eq.process(buf.data() + k * block * 2, block, 2);
    }
    for (float v : buf) {
        CHECK(std::isfinite(v));
        CHECK(std::fabs(v) < 10.0f);
    }
}

// Low-rate local material (22.05 kHz audiobooks, 32 kHz): the 16 kHz band is at or
// above Nyquist there. It must pass through, not become an unstable filter.
TEST(GraphicEq, BandAboveNyquistPassesThrough) {
    for (double fs : {22050.0, 32000.0}) {
        dsp::EqProcessor eq(48000);
        dsp::EqConfig cfg = dsp::defaultGraphicEq();
        cfg.bands.back().gainDb = 12.0;  // the 16 kHz band, boosted
        eq.setConfig(cfg);
        eq.setSampleRate(fs);
        std::vector<float> x(8192);
        uint32_t seed = 1;
        for (float& v : x) {  // white-ish noise excites everything up to Nyquist
            seed = seed * 1664525u + 1013904223u;
            v = float(int32_t((seed >> 8) % 40001u) - 20000) / 100000.0f;  // +-0.2
        }
        eq.process(x.data(), x.size(), 1);
        float peak = 0.0f;
        for (float v : x) {
            CHECK(std::isfinite(v));
            peak = std::max(peak, std::fabs(v));
        }
        CHECK(peak < 1.0f);  // flat elsewhere: the input peaks at 0.2
    }
    const dsp::BiquadCoeffs c =
        dsp::BiquadCoeffs::design(dsp::FilterType::Peaking, 16000, 32000, 12.0, 1.4);
    CHECK_EQ(c.b0, 1.0f);
    CHECK_EQ(c.a1, 0.0f);
}

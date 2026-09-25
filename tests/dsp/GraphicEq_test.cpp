// GraphicEq — the default EQ band layout the device seeds into the EQ tab and the
// engine EQ (review P5: the EQ tab was blank because nothing ever gave it bands).
#include "dsp/GraphicEq.h"

#include <algorithm>
#include <cmath>

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
        CHECK(b.freqHz < 44100.0 / 2.0);    // below Nyquist at every output rate
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

// GraphicEq — the head unit's default EQ band layout, and the rule for which EQ a
// driver profile runs.
//
// EqScreen draws one column per band of its EqConfig and EqProcessor runs one biquad
// per band; an EqConfig with no bands means a blank EQ tab and a bypassed EQ. The
// device seeds this layout (or the active profile's own curve) into both before the
// UI loop, so the EQ tab is always usable.
#pragma once

#include <cstddef>

#include "dsp/Eq.h"

namespace carsh::dsp {

// Classic 10-band octave graphic EQ: peaking filters at the ISO octave centres
// 31.5 Hz .. 16 kHz, flat (0 dB), Q = sqrt(2) (one octave wide, so neighbouring
// bands overlap smoothly). The engine EQ is designed at the output's current rate
// (the kernel calls EqProcessor::setSampleRate() whenever it reclocks the jack),
// so each band acts at its labelled frequency. 16 kHz is below Nyquist at 44.1 kHz
// and up; on lower-rate material (32 kHz and below) the top band can't exist and
// is passed through (BiquadCoeffs::design).
inline EqConfig defaultGraphicEq() {
    static constexpr double kCentresHz[] = {31.5, 63,   125,  250,  500,
                                            1000, 2000, 4000, 8000, 16000};
    EqConfig cfg;
    cfg.preampDb = 0.0;
    for (double f : kCentresHz) {
        EqBand b;
        b.type = FilterType::Peaking;
        b.freqHz = f;
        b.gainDb = 0.0;
        b.q = 1.41421356237;
        cfg.bands.push_back(b);
    }
    return cfg;
}

// The EQ to run for a profile: its stored curve when it has one (clamped to the
// bands EqProcessor can run, so the screen never shows a band the DSP ignores),
// otherwise the default layout.
inline EqConfig eqForProfile(const EqConfig& stored) {
    if (stored.bands.empty()) return defaultGraphicEq();
    EqConfig cfg = stored;
    if (cfg.bands.size() > std::size_t(EqProcessor::kMaxBands))
        cfg.bands.resize(std::size_t(EqProcessor::kMaxBands));
    return cfg;
}

}  // namespace carsh::dsp

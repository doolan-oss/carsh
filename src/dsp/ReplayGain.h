// ReplayGain / R128 application helpers (PLAN.md §5.5).
//
// Loudness *measurement* (BS.1770 integrated LUFS) happens in the indexer on
// import; this module turns the stored gain/peak into the linear gain the engine
// applies as its first stage, in track or album mode, peak-clamped so a positive
// gain can never clip.
#pragma once

#include <algorithm>
#include <cmath>

namespace carsh::dsp {

// EBU R128 / ReplayGain 2.0 reference loudness.
inline constexpr double kReferenceLufs = -23.0;

inline float dbToLinear(double db) { return float(std::pow(10.0, db / 20.0)); }
inline double linearToDb(double lin) {
    return lin > 0 ? 20.0 * std::log10(lin) : -120.0;
}

// Gain (dB) needed to bring a track measured at `measuredLufs` to the reference.
inline double gainToReference(double measuredLufs) {
    return kReferenceLufs - measuredLufs;
}

enum class RgMode { Off, Track, Album };

struct RgInfo {
    float trackGainDb = 0;
    float albumGainDb = 0;
    float trackPeak = 1;   // linear, 0..1+ (sample peak)
    float albumPeak = 1;
};

// Compute the linear gain to apply, peak-limited so output stays <= preventClip
// full-scale. Returns 1.0 when mode is Off.
inline float resolveGain(const RgInfo& info, RgMode mode,
                         double preGainHeadroomDb = 0.0,
                         float ceiling = 1.0f) {
    if (mode == RgMode::Off) return 1.0f;
    double gainDb =
        (mode == RgMode::Album ? info.albumGainDb : info.trackGainDb) +
        preGainHeadroomDb;
    float peak = mode == RgMode::Album ? info.albumPeak : info.trackPeak;
    float lin = dbToLinear(gainDb);
    // Clamp so lin*peak <= ceiling (avoids clipping on positive gains).
    if (peak > 0.0f && lin * peak > ceiling)
        lin = ceiling / peak;
    return lin;
}

}  // namespace carsh::dsp

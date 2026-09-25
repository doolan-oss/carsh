// Biquad — RBJ "Audio EQ Cookbook" second-order section (PLAN.md §2.5).
//
// Computes normalized coefficients (a0 == 1) for peaking, low/high shelf, and
// low/high pass; runs as Transposed Direct Form II, which is the well-behaved
// float topology (low coefficient sensitivity, two state words per channel).
//
// Pure math, ~100 lines, NEON-vectorizable later. The magnitude-response helper
// is used by tests to check the filters against the analytic transfer function.
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>

namespace carsh::dsp {

enum class FilterType : uint8_t {
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowPass = 3,
    HighPass = 4,
};

// Normalized coefficients (a0 folded to 1).
struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0;  // feed-forward
    float a1 = 0, a2 = 0;          // feed-back (a0 == 1)

    static BiquadCoeffs identity() { return {}; }

    // A corner at or above Nyquist (sampleRate / 2) can't be realised at this rate
    // (past it sin(w0) < 0 and the RBJ poles leave the unit circle), so such a band
    // is passed through (identity) rather than designed unstable. That happens when
    // a fixed layout runs at a low output rate, e.g. a 16 kHz band on 22.05 kHz audio.
    static BiquadCoeffs design(FilterType type, double freqHz, double sampleRate,
                               double gainDb, double q) {
        BiquadCoeffs c;
        if (sampleRate <= 0 || freqHz <= 0 || q <= 0) return c;
        if (freqHz >= sampleRate / 2) return c;
        const double w0 = 2.0 * M_PI * freqHz / sampleRate;
        const double cosw = std::cos(w0);
        const double sinw = std::sin(w0);
        const double alpha = sinw / (2.0 * q);
        const double A = std::pow(10.0, gainDb / 40.0);  // shelf/peak amplitude
        double b0, b1, b2, a0, a1, a2;
        switch (type) {
            case FilterType::Peaking:
                b0 = 1 + alpha * A; b1 = -2 * cosw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cosw; a2 = 1 - alpha / A;
                break;
            case FilterType::LowShelf: {
                const double tsa = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cosw + tsa);
                b1 = 2 * A * ((A - 1) - (A + 1) * cosw);
                b2 = A * ((A + 1) - (A - 1) * cosw - tsa);
                a0 = (A + 1) + (A - 1) * cosw + tsa;
                a1 = -2 * ((A - 1) + (A + 1) * cosw);
                a2 = (A + 1) + (A - 1) * cosw - tsa;
                break;
            }
            case FilterType::HighShelf: {
                const double tsa = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cosw + tsa);
                b1 = -2 * A * ((A - 1) + (A + 1) * cosw);
                b2 = A * ((A + 1) + (A - 1) * cosw - tsa);
                a0 = (A + 1) - (A - 1) * cosw + tsa;
                a1 = 2 * ((A - 1) - (A + 1) * cosw);
                a2 = (A + 1) - (A - 1) * cosw - tsa;
                break;
            }
            case FilterType::LowPass:
                b0 = (1 - cosw) / 2; b1 = 1 - cosw; b2 = (1 - cosw) / 2;
                a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
                break;
            case FilterType::HighPass:
                b0 = (1 + cosw) / 2; b1 = -(1 + cosw); b2 = (1 + cosw) / 2;
                a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
                break;
            default:
                return c;
        }
        c.b0 = float(b0 / a0); c.b1 = float(b1 / a0); c.b2 = float(b2 / a0);
        c.a1 = float(a1 / a0); c.a2 = float(a2 / a0);
        return c;
    }

    // |H(e^jw)| at frequency f — the analytic magnitude response (for tests and
    // EQ-curve display).
    double magnitudeAt(double freqHz, double sampleRate) const {
        const double w = 2.0 * M_PI * freqHz / sampleRate;
        const std::complex<double> z1 = std::exp(std::complex<double>(0, -w));
        const std::complex<double> z2 = z1 * z1;
        const std::complex<double> num =
            double(b0) + double(b1) * z1 + double(b2) * z2;
        const std::complex<double> den = 1.0 + double(a1) * z1 + double(a2) * z2;
        return std::abs(num / den);
    }
};

// Per-channel TDF-II state. One instance per (band, channel).
struct BiquadState {
    float s1 = 0, s2 = 0;
    void reset() { s1 = s2 = 0; }

    inline float process(const BiquadCoeffs& c, float x) {
        const float y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

}  // namespace carsh::dsp

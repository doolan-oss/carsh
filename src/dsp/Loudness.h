// Integrated loudness per ITU-R BS.1770-4 / EBU R128 (PLAN.md §5.5).
//
// The indexer streams decoded blocks through this to compute a track's loudness;
// gainToReference() then yields the ReplayGain value stored in the DB. Pipeline:
//   K-weighting (high-shelf "pre" filter + RLB high-pass)
//   -> 400 ms gating blocks, 75% overlap (100 ms hop)
//   -> absolute gate at -70 LUFS, then relative gate at integrated - 10 LU
//   -> integrated = -0.691 + 10·log10(gated mean of channel-summed mean-square).
//
// Filter coefficients are recomputed for the actual sample rate (Brecht De Man
// parameterization of the BS.1770 prototype), so it is correct at 44.1k as well
// as the spec's 48k reference.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/Biquad.h"

namespace carsh::dsp {

class LoudnessMeter {
public:
    LoudnessMeter(double sampleRate, int channels)
        : channels_(channels < 1 ? 1 : channels),
          subLen_(std::size_t(sampleRate * 0.1 + 0.5)) {
        buildKWeighting(sampleRate);
        state_.resize(std::size_t(channels_));
        subAcc_.assign(std::size_t(channels_), 0.0);
    }

    // K-weighting combined magnitude response (for tests / sanity).
    double kMagnitudeAt(double freqHz, double sampleRate) const {
        return pre_.magnitudeAt(freqHz, sampleRate) *
               rlb_.magnitudeAt(freqHz, sampleRate);
    }

    void process(const float* interleaved, std::size_t frames) {
        for (std::size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < channels_; ++c) {
                float x = interleaved[f * std::size_t(channels_) + c];
                if (std::fabs(x) > peak_) peak_ = std::fabs(x);
                float w = state_[std::size_t(c)].rlb.process(
                    rlb_, state_[std::size_t(c)].pre.process(pre_, x));
                subAcc_[std::size_t(c)] += double(w) * w;
            }
            if (++subCount_ >= subLen_) flushSubblock();
        }
    }

    float samplePeak() const { return peak_; }

    // Finalize: returns integrated loudness in LUFS (or -inf-ish if silent).
    double integratedLufs() const {
        // Build 400 ms block energies from 4 consecutive 100 ms sub-blocks.
        std::vector<double> blockEnergy;  // channel-summed mean-square per block
        std::size_t nsub = subMs_.size() / std::size_t(channels_);
        for (std::size_t b = 3; b < nsub; ++b) {
            double s = 0;
            for (int c = 0; c < channels_; ++c) {
                double ms = 0;
                for (std::size_t k = 0; k < 4; ++k)
                    ms += subMs_[(b - k) * std::size_t(channels_) +
                                 std::size_t(c)];
                ms *= 0.25;
                s += channelWeight(c) * ms;
            }
            blockEnergy.push_back(s);
        }
        if (blockEnergy.empty()) return -70.0;

        auto loud = [](double e) { return -0.691 + 10.0 * std::log10(e + 1e-12); };
        // Absolute gate at -70 LUFS.
        std::vector<double> kept;
        for (double e : blockEnergy)
            if (loud(e) > -70.0) kept.push_back(e);
        if (kept.empty()) return -70.0;
        double meanAbs = 0;
        for (double e : kept) meanAbs += e;
        meanAbs /= double(kept.size());
        double relThresh = loud(meanAbs) - 10.0;
        // Relative gate.
        double sum = 0;
        std::size_t cnt = 0;
        for (double e : blockEnergy)
            if (loud(e) > -70.0 && loud(e) > relThresh) { sum += e; ++cnt; }
        if (cnt == 0) return -70.0;
        return loud(sum / double(cnt));
    }

private:
    struct ChState { BiquadState pre, rlb; };

    static double channelWeight(int) { return 1.0; }  // L/R/C = 1.0 (no surround)

    void flushSubblock() {
        for (int c = 0; c < channels_; ++c) {
            subMs_.push_back(subAcc_[std::size_t(c)] / double(subLen_));
            subAcc_[std::size_t(c)] = 0;
        }
        subCount_ = 0;
    }

    void buildKWeighting(double fs) {
        // Stage 1: high-shelf "pre" filter (~+4 dB above ~1.7 kHz).
        {
            const double db = 3.999843853973347;
            const double f0 = 1681.974450955533;
            const double Q = 0.7071752369554196;
            const double K = std::tan(M_PI * f0 / fs);
            const double Vh = std::pow(10.0, db / 20.0);
            const double Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1 + K / Q + K * K;
            pre_.b0 = float((Vh + Vb * K / Q + K * K) / a0);
            pre_.b1 = float(2 * (K * K - Vh) / a0);
            pre_.b2 = float((Vh - Vb * K / Q + K * K) / a0);
            pre_.a1 = float(2 * (K * K - 1) / a0);
            pre_.a2 = float((1 - K / Q + K * K) / a0);
        }
        // Stage 2: RLB high-pass at ~38 Hz.
        {
            const double f0 = 38.13547087602444;
            const double Q = 0.5003270373238773;
            const double K = std::tan(M_PI * f0 / fs);
            const double a0 = 1 + K / Q + K * K;
            rlb_.b0 = 1.0f;
            rlb_.b1 = -2.0f;
            rlb_.b2 = 1.0f;
            rlb_.a1 = float(2 * (K * K - 1) / a0);
            rlb_.a2 = float((1 - K / Q + K * K) / a0);
        }
    }

    int channels_;
    std::size_t subLen_;
    BiquadCoeffs pre_, rlb_;
    std::vector<ChState> state_;
    std::vector<double> subAcc_;
    std::vector<double> subMs_;  // flattened [subblock][channel] mean-squares
    std::size_t subCount_ = 0;
    float peak_ = 0;
};

}  // namespace carsh::dsp

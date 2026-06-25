// Parametric EQ — a cascade of RBJ biquads with a preamp (PLAN.md §2.5/Phase 4).
//
// Implements ISampleProcessor so it drops into the engine chain. The real-time
// subtlety is coefficient swap without zipper noise: we keep two coefficient
// banks and flip an atomic index. process() (audio context) only ever reads the
// active bank; setConfig() (control context) fills the inactive bank and then
// publishes it. Filter state persists across swaps so the cascade keeps running.
//
// A hard bank swap can still click on a large change; the production path can
// cross-fade banks (PLAN.md). Host build verifies math + stability; the click-
// free cross-fade is a small future refinement noted in STATUS.
#pragma once

#include <atomic>
#include <vector>

#include "audio/ISampleProcessor.h"
#include "dsp/Biquad.h"

namespace carsh::dsp {

struct EqBand {
    FilterType type = FilterType::Peaking;
    double freqHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;
};

struct EqConfig {
    double preampDb = 0.0;
    std::vector<EqBand> bands;
};

class EqProcessor : public carsh::audio::ISampleProcessor {
public:
    static constexpr int kMaxBands = 12;
    static constexpr int kMaxChannels = 2;

    explicit EqProcessor(double sampleRate) : sampleRate_(sampleRate) {}

    // Recompute coefficients into the inactive bank and publish atomically.
    // Safe to call from the control task while process() runs on the audio task.
    void setConfig(const EqConfig& cfg) {
        int inactive = 1 - active_.load(std::memory_order_relaxed);
        Bank& b = banks_[inactive];
        int n = int(cfg.bands.size());
        if (n > kMaxBands) n = kMaxBands;
        b.n = n;
        b.preampLin = float(std::pow(10.0, cfg.preampDb / 20.0));
        for (int i = 0; i < n; ++i) {
            const EqBand& bd = cfg.bands[std::size_t(i)];
            b.coeffs[i] =
                BiquadCoeffs::design(bd.type, bd.freqHz, sampleRate_, bd.gainDb,
                                     bd.q);
        }
        active_.store(inactive, std::memory_order_release);
        configured_.store(true, std::memory_order_release);
    }

    void reset() {
        for (auto& ch : state_)
            for (auto& s : ch) s.reset();
    }

    // Analytic combined magnitude response at a frequency (for the EQ curve UI
    // and tests): preamp * product of band magnitudes.
    double magnitudeAt(double freqHz) const {
        int a = active_.load(std::memory_order_acquire);
        const Bank& b = banks_[a];
        double m = b.preampLin;
        for (int i = 0; i < b.n; ++i)
            m *= b.coeffs[i].magnitudeAt(freqHz, sampleRate_);
        return m;
    }

    // ISampleProcessor.
    void process(float* x, std::size_t frames, uint8_t channels) override {
        if (!configured_.load(std::memory_order_acquire)) return;
        int a = active_.load(std::memory_order_acquire);
        const Bank& b = banks_[a];
        const int ch = channels < kMaxChannels ? channels : kMaxChannels;
        for (std::size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < ch; ++c) {
                float v = x[f * channels + c] * b.preampLin;
                for (int i = 0; i < b.n; ++i)
                    v = state_[c][i].process(b.coeffs[i], v);
                x[f * channels + c] = v;
            }
        }
    }

private:
    struct Bank {
        int n = 0;
        float preampLin = 1.0f;
        BiquadCoeffs coeffs[kMaxBands];
    };

    double sampleRate_;
    Bank banks_[2];
    std::atomic<int> active_{0};
    std::atomic<bool> configured_{false};
    // state_[channel][band]
    BiquadState state_[kMaxChannels][kMaxBands];
};

}  // namespace carsh::dsp

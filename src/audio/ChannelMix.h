// ChannelMix — maps a decoder's interleaved frames onto the output channel count.
//
// The ring, engine and sound device run at ONE channel count (stereo on the Pi),
// but files arrive mono, stereo or multichannel, and every decoder emits frames
// in the file's own count (IDecoder contract). The DecodePump runs each decoded
// block through this on its way into the ring:
//
//   * same count  -> copied as-is (bit-exact passthrough)
//   * mono -> 2+  -> the mono signal on both L and R, at unity
//   * 3..8 -> 2+  -> the standard ITU-R BS.775 stereo downmix: centre and the
//                    surround/back channels at -3 dB, LFE dropped, then scaled so
//                    a full-scale signal on every input still cannot clip
//   * any  -> 1   -> the average of that stereo pair
//   * outputs past the first two (a >2-channel device fed a different count)
//     are left silent
//
// Input layouts follow the FLAC / WAVE_FORMAT_EXTENSIBLE default channel order
// (what dr_flac and dr_wav hand back for 1..8 channels). Wider streams are not
// guessed at: supported() rejects them so the player refuses the file cleanly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace carsh::audio {

class ChannelMix {
public:
    static constexpr uint8_t kMaxIn = 8;

    // True when a decoder with `in` channels can be mapped (to any output count).
    static bool supported(uint8_t in) { return in >= 1 && in <= kMaxIn; }

    // Prepares the in->out mapping. Returns false (mix left unconfigured) when
    // either count is unusable.
    bool configure(uint8_t in, uint8_t out) {
        in_ = out_ = 0;
        if (!supported(in) || out == 0) return false;
        for (uint8_t i = 0; i < kMaxIn; ++i) gl_[i] = gr_[i] = 0.0f;
        float sumL = 0.0f, sumR = 0.0f;
        for (uint8_t i = 0; i < in; ++i) {
            const Gain g = gainFor(role(in, i));
            gl_[i] = g.l;
            gr_[i] = g.r;
            sumL += g.l;
            sumR += g.r;
        }
        // Normalise so the loudest side's coefficients sum to at most 1: a
        // full-scale, in-phase signal on every input then stays within [-1, 1].
        const float peak = sumL > sumR ? sumL : sumR;
        if (peak > 1.0f)
            for (uint8_t i = 0; i < in; ++i) {
                gl_[i] /= peak;
                gr_[i] /= peak;
            }
        in_ = in;
        out_ = out;
        return true;
    }

    uint8_t in() const { return in_; }
    uint8_t out() const { return out_; }
    bool configured() const { return in_ != 0; }

    // Converts `frames` frames. `src` holds frames*in() floats, `dst` receives
    // frames*out() floats; the two must not overlap unless in() == out().
    void apply(const float* src, float* dst, std::size_t frames) const {
        if (in_ == out_) {
            if (dst != src) std::memmove(dst, src, frames * in_ * sizeof(float));
            return;
        }
        for (std::size_t f = 0; f < frames; ++f) {
            const float* x = src + f * in_;
            float l = 0.0f, r = 0.0f;
            for (uint8_t i = 0; i < in_; ++i) {
                l += gl_[i] * x[i];
                r += gr_[i] * x[i];
            }
            float* y = dst + f * out_;
            if (out_ == 1) {
                y[0] = 0.5f * (l + r);
            } else {
                y[0] = l;
                y[1] = r;
                for (uint8_t k = 2; k < out_; ++k) y[k] = 0.0f;
            }
        }
    }

    // Per-input stereo gains (exposed for tests; valid after configure()).
    float gainL(uint8_t i) const { return i < kMaxIn ? gl_[i] : 0.0f; }
    float gainR(uint8_t i) const { return i < kMaxIn ? gr_[i] : 0.0f; }

private:
    enum class Role : uint8_t { Mono, FL, FR, FC, LFE, BL, BR, BC, SL, SR };
    struct Gain { float l, r; };

    static constexpr float kMinus3dB = 0.70710678f;

    // Default channel order per count (FLAC spec / WAVEFORMATEXTENSIBLE masks
    // 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F).
    static Role role(uint8_t count, uint8_t i) {
        static constexpr Role k3[] = {Role::FL, Role::FR, Role::FC};
        static constexpr Role k4[] = {Role::FL, Role::FR, Role::BL, Role::BR};
        static constexpr Role k5[] = {Role::FL, Role::FR, Role::FC, Role::BL,
                                      Role::BR};
        static constexpr Role k6[] = {Role::FL, Role::FR, Role::FC, Role::LFE,
                                      Role::BL, Role::BR};
        static constexpr Role k7[] = {Role::FL, Role::FR, Role::FC, Role::LFE,
                                      Role::BC, Role::SL, Role::SR};
        static constexpr Role k8[] = {Role::FL, Role::FR, Role::FC, Role::LFE,
                                      Role::BL, Role::BR, Role::SL, Role::SR};
        switch (count) {
            case 1: return Role::Mono;
            case 2: return i == 0 ? Role::FL : Role::FR;
            case 3: return k3[i];
            case 4: return k4[i];
            case 5: return k5[i];
            case 6: return k6[i];
            case 7: return k7[i];
            default: return k8[i];
        }
    }

    static Gain gainFor(Role r) {
        switch (r) {
            case Role::Mono: return {1.0f, 1.0f};
            case Role::FL: return {1.0f, 0.0f};
            case Role::FR: return {0.0f, 1.0f};
            case Role::FC: return {kMinus3dB, kMinus3dB};
            case Role::LFE: return {0.0f, 0.0f};
            case Role::BL:
            case Role::SL: return {kMinus3dB, 0.0f};
            case Role::BR:
            case Role::SR: return {0.0f, kMinus3dB};
            case Role::BC: return {kMinus3dB * kMinus3dB, kMinus3dB * kMinus3dB};
        }
        return {0.0f, 0.0f};
    }

    uint8_t in_ = 0;
    uint8_t out_ = 0;
    float gl_[kMaxIn] = {};
    float gr_[kMaxIn] = {};
};

}  // namespace carsh::audio

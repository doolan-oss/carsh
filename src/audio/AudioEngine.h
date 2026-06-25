// AudioEngine — the pull-callback host and fixed-cost DSP chain (PLAN.md §2.4/§5.3).
//
// Implements hal::IAudioSink: the sound device pulls float frames from here, and
// here we pull already-decoded PCM from the PcmRing and apply the per-block DSP
// chain. Per PLAN.md §5.3 the order is:
//
//   ring PCM (float) -> ReplayGain gain -> [EQ/sample processor] -> master volume
//                    -> (soft limiter) -> device word size (done by the device)
//
// The callback is fixed-cost and never allocates, locks, or touches the FS — it
// only reads the SPSC ring and runs arithmetic, matching the IRQ/DMA contract.
// A short read from the ring is an underrun: we return the real frame count (the
// device zero-fills the rest) and bump a counter so tests/telemetry can see it.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

#include "audio/PcmRing.h"
#include "hal/ISoundDevice.h"

namespace carsh::audio {

// Optional in-place processor inserted between ReplayGain and master volume.
// The parametric EQ (Phase 4) implements this. Must be real-time safe.
class ISampleProcessor {
public:
    virtual ~ISampleProcessor() = default;
    // Process `frames` interleaved frames in place; `channels` samples/frame.
    virtual void process(float* interleaved, std::size_t frames,
                         uint8_t channels) = 0;
};

class AudioEngine : public carsh::hal::IAudioSink {
public:
    // The ring is owned by the caller (the decode pump writes it). `channels`
    // must match what the device is opened with.
    AudioEngine(PcmRing<float>* ring, uint8_t channels)
        : ring_(ring), channels_(channels) {}

    // --- Control (called from the controller task, not the callback) --------

    // Master volume in linear [0,1]. Stored atomically; read each block.
    void setMasterVolume(float v) {
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        masterVol_.store(v, std::memory_order_relaxed);
    }
    float masterVolume() const { return masterVol_.load(std::memory_order_relaxed); }

    // ReplayGain as linear gain (convert from dB at the call site). Applied as
    // the first stage so EQ/volume act on the loudness-normalized signal.
    void setReplayGainLinear(float g) {
        replayGain_.store(g, std::memory_order_relaxed);
    }

    // Insert/replace the EQ (or any) sample processor. Pointer is read each
    // block via acquire; swap is a single atomic store (real-time-safe).
    void setSampleProcessor(ISampleProcessor* p) {
        processor_.store(p, std::memory_order_release);
    }

    // Enable a simple post-chain soft limiter (tanh) to tame EQ-boost clipping.
    void setSoftLimiter(bool on) { limiter_.store(on, std::memory_order_relaxed); }

    // Pause gates the callback: it outputs silence and does NOT drain the ring,
    // so the buffered audio (and thus playback position) is preserved on resume.
    void setPaused(bool p) { paused_.store(p, std::memory_order_relaxed); }
    bool paused() const { return paused_.load(std::memory_order_relaxed); }

    std::size_t underrunFrames() const {
        return underrun_.load(std::memory_order_relaxed);
    }
    void resetUnderrunCount() { underrun_.store(0, std::memory_order_relaxed); }

    // Total real (non-underrun) frames delivered to the device — the basis for
    // the playback position the UI shows.
    std::uint64_t framesRendered() const {
        return rendered_.load(std::memory_order_relaxed);
    }
    void resetRenderedCount() { rendered_.store(0, std::memory_order_relaxed); }

    // --- IAudioSink (the pull callback) ------------------------------------

    std::size_t render(float* out, std::size_t frames) override {
        if (paused_.load(std::memory_order_relaxed)) {
            for (std::size_t i = 0; i < frames * channels_; ++i) out[i] = 0.0f;
            return frames;  // silence, but ring untouched and not an underrun
        }
        const std::size_t want = frames * channels_;
        const std::size_t got = ring_->read(out, want);
        const std::size_t framesGot = got / channels_;
        rendered_.fetch_add(framesGot, std::memory_order_relaxed);
        if (framesGot < frames)
            underrun_.fetch_add(frames - framesGot, std::memory_order_relaxed);

        // Stage 1: ReplayGain (linear).
        const float rg = replayGain_.load(std::memory_order_relaxed);
        if (rg != 1.0f)
            for (std::size_t i = 0; i < got; ++i) out[i] *= rg;

        // Stage 2: EQ / sample processor (in place).
        if (ISampleProcessor* p = processor_.load(std::memory_order_acquire))
            p->process(out, framesGot, channels_);

        // Stage 3: master volume.
        const float mv = masterVol_.load(std::memory_order_relaxed);
        if (mv != 1.0f)
            for (std::size_t i = 0; i < got; ++i) out[i] *= mv;

        // Stage 4 (optional): soft limiter.
        if (limiter_.load(std::memory_order_relaxed))
            for (std::size_t i = 0; i < got; ++i) out[i] = softClip(out[i]);

        return framesGot;
    }

    uint8_t channels() const { return channels_; }

private:
    static float softClip(float x) {
        // Soft-knee limiter: pass-through below the threshold, then a smooth
        // tanh knee that stays bounded inside (-1, 1). Prevents the harsh edge
        // of a hard clamp on EQ-boosted peaks while leaving normal levels exact.
        constexpr float t = 0.9f;  // knee threshold
        float a = std::fabs(x);
        if (a <= t) return x;
        float over = (a - t) / (1.0f - t);
        float shaped = t + (1.0f - t) * std::tanh(over);
        return x < 0 ? -shaped : shaped;
    }

    PcmRing<float>* ring_;
    uint8_t channels_;
    std::atomic<float> masterVol_{1.0f};
    std::atomic<float> replayGain_{1.0f};
    std::atomic<ISampleProcessor*> processor_{nullptr};
    std::atomic<bool> limiter_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::size_t> underrun_{0};
    std::atomic<std::uint64_t> rendered_{0};
};

}  // namespace carsh::audio

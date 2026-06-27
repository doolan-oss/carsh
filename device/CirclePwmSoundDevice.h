// CirclePwmSoundDevice — device backend for hal::ISoundDevice over Circle's
// CPWMSoundBaseDevice, driving the Pi's onboard 3.5mm headphone jack (GPIO 41/40
// routed to the jack on the Pi 4). This is the no-extra-hardware bring-up output:
// low-fi (PWM is ~11-bit, audibly noisy) but proves the full decode -> ring ->
// engine -> DSP path makes sound. A USB/I2S DAC is the production upgrade behind
// this same interface (see CircleSoundDevice for the I2S variant).
//
// The PWM DMA pulls words via GetChunk() (SoundFormatUnsigned32 -> the u32
// overload), off the IRQ — so when this device is open it is the SOLE ring
// consumer (the engine's SPSC contract: decode task produces, this IRQ consumes).
#ifndef _carsh_circle_pwm_sound_device_h
#define _carsh_circle_pwm_sound_device_h

#include <circle/interrupt.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/timer.h>
#include <circle/types.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "hal/ISoundDevice.h"

namespace carsh::device {

// Inner Circle device: pulls float frames from the sink and maps [-1,1] onto the
// PWM's UNSIGNED range (silence = midpoint), unlike the signed I2S path. Also
// tracks the output peak for the on-screen level meter.
class CarshPwmDevice : public CPWMSoundBaseDevice {
public:
    static constexpr unsigned kChunk = 2048;  // words per GetChunk()

    CarshPwmDevice(CInterruptSystem* irq, unsigned sampleRate, uint8_t channels,
                   carsh::hal::IAudioSink* sink)
        : CPWMSoundBaseDevice(irq, sampleRate, kChunk, TRUE),
          sink_(sink),
          channels_(channels),
          scratch_(kChunk) {}  // pre-sized: GetChunk() must NOT allocate (IRQ ctx)

    unsigned GetChunk(u32* pBuffer, unsigned nChunkSize) override {
        const int rmin = GetRangeMin(), rmax = GetRangeMax();
        const float mid = (float(rmin) + float(rmax)) * 0.5f;
        const float amp = (float(rmax) - float(rmin)) * 0.5f;

        const unsigned frames = nChunkSize / channels_;
        if (scratch_.size() < nChunkSize) scratch_.resize(nChunkSize);  // safety only
        const std::size_t got = sink_->render(scratch_.data(), frames);
        const unsigned valid = unsigned(got) * channels_;

        float peak = 0.0f;
        for (unsigned i = 0; i < nChunkSize; ++i) {
            float f = i < valid ? scratch_[i] : 0.0f;  // zero-fill underrun
            if (f > 1.0f) f = 1.0f;
            else if (f < -1.0f) f = -1.0f;
            if (i < valid) { float a = std::fabs(f); if (a > peak) peak = a; }
            int v = int(mid + f * amp);
            if (v < rmin) v = rmin;
            else if (v > rmax) v = rmax;
            pBuffer[i] = u32(v);
        }
        peak_.store(peak, std::memory_order_relaxed);
        return nChunkSize;
    }

    float peak() const { return peak_.load(std::memory_order_relaxed); }

private:
    carsh::hal::IAudioSink* sink_;
    uint8_t channels_;
    std::vector<float> scratch_;
    std::atomic<float> peak_{0.0f};
};

class CirclePwmSoundDevice : public carsh::hal::ISoundDevice {
public:
    explicit CirclePwmSoundDevice(CInterruptSystem* irq) : irq_(irq) {}
    ~CirclePwmSoundDevice() override { close(); }

    bool open(const carsh::audio::AudioFormat& fmt,
              carsh::hal::IAudioSink* sink) override {
        if (!fmt.valid() || !sink) return false;
        close();
        fmt_ = fmt;
        dev_ = new CarshPwmDevice(irq_, fmt.sampleRate, fmt.channels, sink);
        open_ = dev_->Start();
        return open_;
    }
    void close() override {
        if (dev_) {
            // Drain the DMA before destroying: deleting an active device leaves an
            // in-flight completion IRQ pointing at freed buffers (a hard lock-up).
            // Cancel stops it after the current buffer; wait (bounded) for that.
            dev_->Cancel();
            for (unsigned i = 0; i < 200 && dev_->IsActive(); ++i)
                CTimer::SimpleMsDelay(1);
            delete dev_;
            dev_ = nullptr;
        }
        open_ = false;
    }
    carsh::audio::AudioFormat format() const override { return fmt_; }
    bool isOpen() const override { return open_; }

    // Output peak [0,1] for the diagnostics level meter (0 when no device).
    float peak() const { return dev_ ? dev_->peak() : 0.0f; }

private:
    CInterruptSystem* irq_;
    CarshPwmDevice* dev_ = nullptr;
    carsh::audio::AudioFormat fmt_{};
    bool open_ = false;
};

}  // namespace carsh::device

#endif

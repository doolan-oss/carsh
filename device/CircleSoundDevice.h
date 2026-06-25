// CircleSoundDevice — device backend for hal::ISoundDevice over Circle's
// CI2SSoundBaseDevice (PLAN.md §2.4). The I2S DMA pulls sample words via the
// overridden GetChunk(); we pull float frames from the bound IAudioSink (the
// AudioEngine) and scale to the hardware range. This is the production path for
// the HiFiBerry DAC+ (PCM5122). A PWM/jack variant would subclass
// CPWMSoundBaseDevice behind the same interface.
//
// NOTE: constructing/Start()ing this touches real I2S clocks + DMA, so it is only
// activated on hardware (not in the QEMU boot demo). It links here so the whole
// audio path is built on-device; real-time behaviour is validated at the bench
// (HARDWARE_TODO H3/H4).
#ifndef _carsh_circle_sound_device_h
#define _carsh_circle_sound_device_h

#include <circle/interrupt.h>
#include <circle/i2cmaster.h>
#include <circle/sound/i2ssoundbasedevice.h>
#include <circle/types.h>

#include <vector>

#include "hal/ISoundDevice.h"

namespace carsh::device {

// Inner Circle device: pulls float frames from the sink and scales to range.
class CarshI2SDevice : public CI2SSoundBaseDevice {
public:
    CarshI2SDevice(CInterruptSystem* irq, unsigned sampleRate, uint8_t channels,
                   carsh::hal::IAudioSink* sink, CI2CMaster* i2c, u8 dacAddr)
        : CI2SSoundBaseDevice(irq, sampleRate, 8192, FALSE, i2c, dacAddr),
          sink_(sink),
          channels_(channels) {}

    unsigned GetChunk(s16* pBuffer, unsigned nChunkSize) override {
        return fill(pBuffer, nChunkSize, 32767);
    }
    unsigned GetChunk(u32* pBuffer, unsigned nChunkSize) override {
        return fill(pBuffer, nChunkSize, GetRangeMax());
    }

private:
    template <typename T>
    unsigned fill(T* out, unsigned nChunkSize, int range) {
        const unsigned frames = nChunkSize / channels_;
        if (scratch_.size() < nChunkSize) scratch_.resize(nChunkSize);
        const std::size_t got = sink_->render(scratch_.data(), frames);
        const unsigned validWords = unsigned(got) * channels_;
        for (unsigned i = 0; i < nChunkSize; ++i) {
            float f = i < validWords ? scratch_[i] : 0.0f;  // zero-fill underrun
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            out[i] = T(int(f * float(range)));
        }
        return nChunkSize;
    }

    carsh::hal::IAudioSink* sink_;
    uint8_t channels_;
    std::vector<float> scratch_;
};

class CircleSoundDevice : public carsh::hal::ISoundDevice {
public:
    CircleSoundDevice(CInterruptSystem* irq, CI2CMaster* i2c = nullptr,
                      u8 dacAddr = 0)
        : irq_(irq), i2c_(i2c), dacAddr_(dacAddr) {}
    ~CircleSoundDevice() override { close(); }

    bool open(const carsh::audio::AudioFormat& fmt,
              carsh::hal::IAudioSink* sink) override {
        if (!fmt.valid() || !sink) return false;
        close();
        fmt_ = fmt;
        dev_ = new CarshI2SDevice(irq_, fmt.sampleRate, fmt.channels, sink, i2c_,
                                  dacAddr_);
        open_ = dev_->Start();
        return open_;
    }
    void close() override {
        if (dev_) { delete dev_; dev_ = nullptr; }
        open_ = false;
    }
    carsh::audio::AudioFormat format() const override { return fmt_; }
    bool isOpen() const override { return open_; }

private:
    CInterruptSystem* irq_;
    CI2CMaster* i2c_;
    u8 dacAddr_;
    CarshI2SDevice* dev_ = nullptr;
    carsh::audio::AudioFormat fmt_{};
    bool open_ = false;
};

}  // namespace carsh::device

#endif

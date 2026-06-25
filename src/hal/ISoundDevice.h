// ISoundDevice — the audio output abstraction (PLAN.md §2.4).
//
// On the Pi this wraps Circle's CPWMSoundBaseDevice (bring-up) or
// CI2SSoundBaseDevice (production PCM5122). On the build server it is backed by
// WavSinkDevice, which pulls the same float frames and writes them to a .wav so
// the whole decode -> ring -> engine -> DSP chain is verifiable by diffing audio.
//
// Contract: the device owns the clock. It repeatedly pulls interleaved float
// frames from an IAudioSink (the AudioEngine), in the device's channel layout,
// each sample nominally in [-1, 1]. The device converts to its word size and
// emits. A short return from render() is an underrun: the device zero-fills the
// remainder and the engine counts it.
#pragma once

#include <cstddef>

#include "audio/AudioFormat.h"

namespace carsh::hal {

class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    // Produce up to `frames` interleaved frames into out (channels per frame,
    // matching the open() format). Returns the number of frames actually
    // produced; fewer than requested signals underrun.
    virtual std::size_t render(float* out, std::size_t frames) = 0;
};

class ISoundDevice {
public:
    virtual ~ISoundDevice() = default;

    // Configure the output clock/format and bind the sink that feeds it.
    // Returns false if the device cannot produce the requested format.
    virtual bool open(const carsh::audio::AudioFormat& fmt, IAudioSink* sink) = 0;

    virtual void close() = 0;

    // The format the device is currently clocked at.
    virtual carsh::audio::AudioFormat format() const = 0;

    // True between a successful open() and close().
    virtual bool isOpen() const = 0;
};

}  // namespace carsh::hal

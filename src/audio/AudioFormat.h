// Shared PCM format description used across decoders, the engine, and devices.
#pragma once

#include <cstdint>

namespace carsh::audio {

struct AudioFormat {
    uint32_t sampleRate = 0;  // Hz, e.g. 44100, 48000, 96000, 192000
    uint8_t bits = 0;         // bits per sample in the *source/device* domain
    uint8_t channels = 0;     // 1 = mono, 2 = stereo

    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate && bits == o.bits &&
               channels == o.channels;
    }
    bool operator!=(const AudioFormat& o) const { return !(*this == o); }
    bool valid() const { return sampleRate > 0 && channels > 0; }
};

}  // namespace carsh::audio

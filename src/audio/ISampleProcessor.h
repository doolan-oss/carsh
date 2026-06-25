// In-place audio sample processor inserted into the engine's DSP chain between
// ReplayGain and master volume (PLAN.md §5.3). The parametric EQ implements this.
// Implementations must be real-time safe: no allocation, locks, or FS in process().
#pragma once

#include <cstddef>
#include <cstdint>

namespace carsh::audio {

class ISampleProcessor {
public:
    virtual ~ISampleProcessor() = default;
    // Process `frames` interleaved frames in place; `channels` samples/frame.
    virtual void process(float* interleaved, std::size_t frames,
                         uint8_t channels) = 0;
};

}  // namespace carsh::audio

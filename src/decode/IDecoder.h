// IDecoder — streaming audio decode abstraction (PLAN.md §2.8).
//
// Decoders read source bytes through hal::IStream (so they work over USB/SD FAT
// on the Pi and over HostVfs on the build server), and emit interleaved float32
// PCM in small blocks. The decode task calls decode() with a small maxFrames and
// yields between calls, so no decoder monopolizes the cooperative scheduler.
//
// Output is always float32 in [-1, 1], `channels` samples per frame — the engine
// works in float (PLAN.md §5.3) and converts to the DAC word size at the very end.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hal/Vfs.h"

namespace carsh::decode {

struct DecodedFormat {
    uint32_t sampleRate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;            // source bit depth (informational; output is float)
    uint64_t totalFrames = 0;   // 0 if unknown/unseekable
    bool valid() const { return sampleRate > 0 && channels > 0; }
};

class IDecoder {
public:
    virtual ~IDecoder() = default;

    // Binds the stream and parses headers. The decoder does NOT own the stream;
    // the caller keeps it alive for the decoder's lifetime. Returns false if the
    // stream is not this decoder's format.
    virtual bool open(carsh::hal::IStream* stream) = 0;

    // Decodes up to maxFrames interleaved frames into out (must hold
    // maxFrames*channels floats). Returns frames produced; 0 means EOF.
    virtual std::size_t decode(float* out, std::size_t maxFrames) = 0;

    // Seeks to an absolute frame index. Returns false if unsupported/out of range.
    virtual bool seek(uint64_t frame) = 0;

    virtual DecodedFormat format() const = 0;

    // Convenience: total duration in ms (0 if unknown).
    uint64_t durationMs() const {
        auto f = format();
        if (!f.valid() || f.totalFrames == 0) return 0;
        return f.totalFrames * 1000ull / f.sampleRate;
    }
};

// Codec tags for sniffing/registration.
enum class Codec : uint8_t { Unknown, Wav, Flac, Mp3, Aac, Alac, Opus };

}  // namespace carsh::decode

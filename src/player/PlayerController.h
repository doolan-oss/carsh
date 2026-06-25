// PlayerController — transport state, queue, gapless sequencing (PLAN.md §2.x,
// Phase 1.5). Owns the decoders/streams for the current and next track, drives
// the DecodePump (the decode-task slice is serviceDecode()), and exposes a
// PlayerState snapshot for the UI.
//
// Position model: the engine counts rendered (heard) frames; the pump records a
// cumulative output-frame boundary at each track end. The number of boundaries
// already passed by the rendered count IS the now-playing index, and the frames
// since that boundary are the in-track position. This stays correct across
// gapless handoffs without the controller needing real-time timers.
//
// Gapless requires matching format between tracks; on a sample-rate change the
// controller declines to preload and the engine would reclock (HW backend) —
// host build runs a single rate, with the mismatch path stubbed (see TODO).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/AudioEngine.h"
#include "audio/DecodePump.h"
#include "audio/PcmRing.h"
#include "decode/IDecoder.h"
#include "hal/Vfs.h"

namespace carsh::player {

enum class Transport : uint8_t { Stopped, Playing, Paused };

struct PlayerState {
    Transport transport = Transport::Stopped;
    int queueIndex = -1;      // now-playing index, -1 if stopped
    std::size_t queueSize = 0;
    std::string path;         // now-playing path
    uint64_t positionMs = 0;
    uint64_t durationMs = 0;  // 0 if unknown (e.g. streamed AAC)
    std::size_t underrunFrames = 0;
};

class PlayerController {
public:
    PlayerController(carsh::hal::Vfs& vfs, carsh::audio::PcmRing<float>& ring,
                     carsh::audio::AudioEngine& engine, uint8_t channels);

    void setQueue(std::vector<std::string> paths);
    void clearQueue();
    const std::vector<std::string>& queue() const { return queue_; }

    // Transport. play() starts at `index` (or current if -1). Returns false if
    // the index is out of range or the file cannot be opened.
    bool play(int index = -1);
    void pause();
    void resume();
    void stop();
    bool next();
    bool prev();

    // Seek within the now-playing track. Returns false if unseekable.
    bool seekMs(uint64_t ms);

    // The decode-task cooperative slice: top up the ring, advance bookkeeping,
    // preload the next track for gapless. Call repeatedly (every scheduler pass).
    void serviceDecode();

    PlayerState state() const;

private:
    struct Loaded {
        std::unique_ptr<carsh::hal::IStream> stream;
        std::unique_ptr<carsh::decode::IDecoder> dec;
        carsh::decode::DecodedFormat fmt;
    };
    Loaded loadIndex(int index);
    bool startAt(int index);
    void preloadNext();
    void advanceHeard();
    uint64_t renderedFrames() const { return engine_.framesRendered(); }

    carsh::hal::Vfs& vfs_;
    carsh::audio::PcmRing<float>& ring_;
    carsh::audio::AudioEngine& engine_;
    uint8_t channels_;
    uint32_t rate_ = 0;

    std::vector<std::string> queue_;
    std::vector<uint64_t> durationMs_;  // per-queue, 0 if unknown/unloaded

    Transport transport_ = Transport::Stopped;
    int heardIndex_ = -1;   // queue index currently being heard
    int fedIndex_ = -1;     // highest queue index handed to the pump

    Loaded cur_;
    Loaded next_;
    std::unique_ptr<carsh::audio::DecodePump> pump_;

    // Position baselines (see header note).
    uint64_t curTrackStartRendered_ = 0;
    uint64_t curTrackStartFrame_ = 0;  // decoder frame the current track began at
    std::size_t boundariesHeard_ = 0;
};

}  // namespace carsh::player

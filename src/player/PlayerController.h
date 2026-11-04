// PlayerController — transport state, queue, gapless sequencing (PLAN.md §2.x,
// Phase 1.5). Owns the decoders/streams for the current and next track, drives
// the DecodePump (the decode-task slice is serviceDecode()), and exposes a
// PlayerState snapshot for the UI.
//
// Position model: the engine counts rendered (heard) frames; the pump records a
// cumulative output-frame boundary at each gapless handoff, and the controller
// remembers which queue index each handoff led to. The boundaries already passed
// by the rendered count give the now-playing index, and the frames since that
// boundary are the in-track position. This stays correct across gapless
// handoffs without the controller needing real-time timers.
//
// Sequencing: the track after the current one is chained into the pump
// (gapless) when its sample rate matches; channel counts may differ (the pump
// mixes every decoder to the output count). Queue entries that fail to open are
// remembered and skipped. A successor at a different rate is loaded once and
// held; when the current track has fully played out, playback restarts on it
// non-gapless (fresh pump at the new rate), and the device reclocks from
// state().sampleRate / the new queueIndex.
//
// Ownership: cur_ is always the decoder the pump is reading and next_ the one
// queued behind it. A handoff can happen inside any fill() (a track shorter
// than the ring's free space drains within startAt()/seekMs()), so every fill
// is followed by syncHandoff(), and a decoder is only released once the pump
// has moved past it or has been destroyed.
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
    uint32_t sampleRate = 0;  // native rate of the audio being heard (0 if none);
                              // changes on a non-gapless restart at a new rate
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
    // the index is out of range or the file cannot be opened / mapped.
    bool play(int index = -1);
    void pause();
    void resume();
    void stop();
    // Skip to the next / previous playable track (queue entries known to be
    // unplayable are passed over). next() wraps to the start of the queue.
    bool next();
    bool prev();

    // Seek within the now-playing track. Returns false if unseekable.
    bool seekMs(uint64_t ms);

    // The decode-task cooperative slice: top up the ring, advance bookkeeping,
    // preload the next track for gapless, and restart on the next playable track
    // when the current one ends without a gapless successor. Call repeatedly
    // (every scheduler pass).
    void serviceDecode();

    PlayerState state() const;

    // Self-check for tests/diagnostics: true when the pump is reading exactly the
    // decoders this controller owns (cur_ as current, next_ as queued) — i.e. no
    // decoder the pump can still touch has been released.
    bool ownershipConsistent() const;

private:
    struct Loaded {
        std::unique_ptr<carsh::hal::IStream> stream;
        std::unique_ptr<carsh::decode::IDecoder> dec;
        carsh::decode::DecodedFormat fmt;
        int index = -1;  // queue index this was loaded from

        Loaded() = default;
        Loaded(Loaded&&) noexcept = default;
        // Release the decoder BEFORE the stream it reads (the implicit
        // memberwise move would close the stream first).
        Loaded& operator=(Loaded&& o) noexcept {
            if (this != &o) {
                dec.reset();
                stream = std::move(o.stream);
                dec = std::move(o.dec);
                fmt = o.fmt;
                index = o.index;
                o.index = -1;
            }
            return *this;
        }
        void release() { *this = Loaded{}; }
    };
    Loaded loadIndex(int index);
    bool startAt(int index);
    bool startLoaded(Loaded l, bool flushRing);
    void newPump();
    void topUp();
    void syncHandoff();
    void preloadNext();
    void advanceHeard();
    bool restartOnSuccessor();
    bool knownUnplayable(int index) const {
        return index >= 0 && std::size_t(index) < unplayable_.size() &&
               unplayable_[std::size_t(index)];
    }
    uint64_t renderedFrames() const { return engine_.framesRendered(); }
    uint64_t renderedByPump() const {
        uint64_t r = renderedFrames();
        return r >= pumpBaseRendered_ ? r - pumpBaseRendered_ : 0;
    }

    carsh::hal::Vfs& vfs_;
    carsh::audio::PcmRing<float>& ring_;
    carsh::audio::AudioEngine& engine_;
    uint8_t channels_;
    uint32_t rate_ = 0;

    std::vector<std::string> queue_;
    std::vector<uint64_t> durationMs_;  // per-queue, 0 if unknown/unloaded
    std::vector<uint8_t> unplayable_;   // per-queue: failed to open/map (skipped
                                        // by auto-advance, preload and next/prev)

    Transport transport_ = Transport::Stopped;
    int heardIndex_ = -1;   // queue index currently being heard

    Loaded cur_;     // the pump's current decoder
    Loaded next_;    // chained into the pump for gapless, or empty
    Loaded staged_;  // successor at a different rate: started non-gapless
    bool successorResolved_ = false;  // successor of cur_ chained/staged/none
    std::unique_ptr<carsh::audio::DecodePump> pump_;
    std::vector<int> chained_;  // queue index entered at each pump boundary

    // Position baselines (see header note). Rendered counts are absolute
    // engine counts; pump boundaries are relative to pumpBaseRendered_.
    uint64_t pumpBaseRendered_ = 0;
    uint64_t curTrackStartRendered_ = 0;
    uint64_t curTrackStartFrame_ = 0;  // decoder frame the current track began at
    std::size_t boundariesHeard_ = 0;
};

}  // namespace carsh::player

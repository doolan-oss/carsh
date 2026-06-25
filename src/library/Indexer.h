// Library Indexer (PLAN.md §2.9) — walks a Source, parses tags + audio props,
// and populates the Database. Interruptible: build a worklist once, then process
// a few files per step() and Yield() between slices on the device so audio never
// glitches. Optional ReplayGain pass decodes each file through the BS.1770 meter
// (track gain) and aggregates per album (album gain).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dsp/Loudness.h"
#include "dsp/ReplayGain.h"
#include "hal/Vfs.h"
#include "library/Database.h"

namespace carsh::library {

class Indexer {
public:
    struct Progress {
        std::size_t filesDone = 0;
        std::size_t filesTotal = 0;
        bool complete = false;
    };

    explicit Indexer(Database& db) : db_(db) {}

    void enableReplayGain(bool on) { doReplayGain_ = on; }

    // Collect the audio files under the source (recursive). Cheap metadata only.
    void beginScan(carsh::hal::Source& src);

    // Process up to maxFiles worklist entries. Returns false when complete.
    // Each call is bounded so it can be interleaved with playback.
    bool step(std::size_t maxFiles = 1);

    Progress progress() const {
        return {done_, work_.size(), done_ >= work_.size()};
    }

private:
    struct WorkItem {
        std::string path;
        std::string folder;
    };
    void finalizeReplayGain();

    Database& db_;
    carsh::hal::Source* src_ = nullptr;
    std::vector<WorkItem> work_;
    std::size_t done_ = 0;
    bool doReplayGain_ = false;
    bool finalized_ = false;

    // Per-album loudness accumulation for album gain.
    struct AlbumAcc {
        std::unique_ptr<carsh::dsp::LoudnessMeter> meter;
        std::vector<uint64_t> trackIds;
    };
    std::map<uint64_t, AlbumAcc> albums_;
};

}  // namespace carsh::library

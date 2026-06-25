#include "library/Indexer.h"

#include <vector>

#include "decode/DecoderFactory.h"
#include "library/tags/TagReader.h"

namespace carsh::library {

using carsh::decode::Codec;

static std::string parentFolder(const std::string& path) {
    auto p = path.find_last_of('/');
    return p == std::string::npos ? "" : path.substr(0, p);
}

void Indexer::beginScan(carsh::hal::Source& src) {
    src_ = &src;
    work_.clear();
    done_ = 0;
    finalized_ = false;
    albums_.clear();

    // Iterative directory walk (no deep recursion on the device stack).
    std::vector<std::string> dirs = {src.root()};
    while (!dirs.empty()) {
        std::string dir = dirs.back();
        dirs.pop_back();
        std::vector<carsh::hal::DirEntry> entries;
        if (!src.vfs().readdir(dir, entries)) continue;
        for (auto& e : entries) {
            std::string child = dir;
            if (child.empty() || child.back() != '/') child += '/';
            child += e.name;
            if (e.isDir) {
                dirs.push_back(child);
            } else if (carsh::decode::codecFromExtension(e.name) !=
                       Codec::Unknown) {
                work_.push_back({child, parentFolder(child)});
            }
        }
    }
}

bool Indexer::step(std::size_t maxFiles) {
    if (!src_) return false;
    for (std::size_t k = 0; k < maxFiles && done_ < work_.size(); ++k) {
        const WorkItem& w = work_[done_];
        auto stream = src_->vfs().open(w.path);
        if (stream) {
            Codec codec = carsh::decode::sniff(stream.get());
            if (codec == Codec::Unknown)
                codec = carsh::decode::codecFromExtension(w.path);

            TrackMeta meta;
            // Audio properties from the decoder (rate/channels/bits/duration).
            auto dec = carsh::decode::createDecoder(codec);
            if (dec && dec->open(stream.get())) {
                auto fmt = dec->format();
                meta.sampleRate = fmt.sampleRate;
                meta.channels = fmt.channels;
                meta.bitsPerSample = fmt.bits;
                meta.durationMs = uint32_t(dec->durationMs());
            }
            // Tags (override/fill text + may refine props for FLAC STREAMINFO).
            stream->seek(0);
            carsh::library::tags::readTags(stream.get(), codec, meta);
            // Title fallback to filename stem if untagged.
            if (meta.title.empty()) {
                std::string base = w.path.substr(w.path.find_last_of('/') + 1);
                auto dot = base.find_last_of('.');
                meta.title = dot == std::string::npos ? base : base.substr(0, dot);
            }

            uint64_t id = db_.addTrack(meta, src_->id(), w.path, uint8_t(codec),
                                       w.folder);

            if (doReplayGain_ && dec) {
                // Decode the whole file through the BS.1770 meter for track gain
                // and feed the per-album accumulator for album gain.
                uint64_t albumKey =
                    makeAlbumKey(meta.effectiveAlbumArtist(), meta.album, w.folder);
                auto& acc = albums_[albumKey];
                if (!acc.meter)
                    acc.meter = std::make_unique<carsh::dsp::LoudnessMeter>(
                        meta.sampleRate ? meta.sampleRate : 44100,
                        meta.channels ? meta.channels : 2);
                acc.trackIds.push_back(id);

                carsh::dsp::LoudnessMeter trackMeter(
                    meta.sampleRate ? meta.sampleRate : 44100,
                    meta.channels ? meta.channels : 2);
                dec->seek(0);
                std::vector<float> buf(4096 * (meta.channels ? meta.channels : 2));
                std::size_t got;
                std::size_t blockFrames = 4096;
                while ((got = dec->decode(buf.data(), blockFrames)) > 0) {
                    trackMeter.process(buf.data(), got);
                    acc.meter->process(buf.data(), got);
                }
                double lufs = trackMeter.integratedLufs();
                float gain = float(carsh::dsp::gainToReference(lufs));
                db_.setReplayGain(id, gain, gain, trackMeter.samplePeak(),
                                  trackMeter.samplePeak());
            }
        }
        ++done_;
    }

    if (done_ >= work_.size() && !finalized_) {
        if (doReplayGain_) finalizeReplayGain();
        db_.buildIndexes();
        finalized_ = true;
        return false;
    }
    return done_ < work_.size();
}

void Indexer::finalizeReplayGain() {
    for (auto& kv : albums_) {
        AlbumAcc& acc = kv.second;
        if (!acc.meter) continue;
        double albumLufs = acc.meter->integratedLufs();
        float albumGain = float(carsh::dsp::gainToReference(albumLufs));
        float albumPeak = acc.meter->samplePeak();
        for (uint64_t id : acc.trackIds) {
            const Track* t = db_.get(id);
            if (!t) continue;
            // Preserve the already-computed track gain/peak; set album fields.
            db_.setReplayGain(id, t->rgTrackGain, albumGain, t->rgTrackPeak,
                              albumPeak);
        }
    }
}

}  // namespace carsh::library

#include "player/PlayerController.h"

#include "decode/DecoderFactory.h"

namespace carsh::player {

using carsh::audio::DecodePump;

PlayerController::PlayerController(carsh::hal::Vfs& vfs,
                                  carsh::audio::PcmRing<float>& ring,
                                  carsh::audio::AudioEngine& engine,
                                  uint8_t channels)
    : vfs_(vfs), ring_(ring), engine_(engine), channels_(channels) {}

void PlayerController::setQueue(std::vector<std::string> paths) {
    stop();
    queue_ = std::move(paths);
    durationMs_.assign(queue_.size(), 0);
}

void PlayerController::clearQueue() {
    stop();
    queue_.clear();
    durationMs_.clear();
}

PlayerController::Loaded PlayerController::loadIndex(int index) {
    Loaded l;
    if (index < 0 || std::size_t(index) >= queue_.size()) return l;
    l.stream = vfs_.open(queue_[std::size_t(index)]);
    if (!l.stream) return l;
    l.dec = carsh::decode::openAuto(l.stream.get());
    if (!l.dec) { l.stream.reset(); return l; }
    l.fmt = l.dec->format();
    durationMs_[std::size_t(index)] = l.dec->durationMs();
    return l;
}

bool PlayerController::startAt(int index) {
    Loaded l = loadIndex(index);
    if (!l.dec) return false;

    // Fresh playback context: reset ring + engine counters.
    ring_.reset();
    engine_.resetRenderedCount();
    engine_.resetUnderrunCount();
    engine_.setPaused(false);

    cur_ = std::move(l);
    next_ = Loaded{};
    rate_ = cur_.fmt.sampleRate;
    pump_ = std::make_unique<DecodePump>(cur_.dec.get(), &ring_, channels_, 512);

    heardIndex_ = index;
    fedIndex_ = index;
    curTrackStartRendered_ = 0;
    curTrackStartFrame_ = 0;
    boundariesHeard_ = 0;
    transport_ = Transport::Playing;
    preloadNext();
    pump_->fill();
    return true;
}

bool PlayerController::play(int index) {
    if (queue_.empty()) return false;
    if (index < 0) index = heardIndex_ < 0 ? 0 : heardIndex_;
    return startAt(index);
}

void PlayerController::pause() {
    if (transport_ == Transport::Playing) {
        transport_ = Transport::Paused;
        engine_.setPaused(true);
    }
}

void PlayerController::resume() {
    if (transport_ == Transport::Paused) {
        transport_ = Transport::Playing;
        engine_.setPaused(false);
    }
}

void PlayerController::stop() {
    transport_ = Transport::Stopped;
    engine_.setPaused(false);
    pump_.reset();
    cur_ = Loaded{};
    next_ = Loaded{};
    ring_.reset();
    heardIndex_ = -1;
    fedIndex_ = -1;
}

bool PlayerController::next() {
    if (heardIndex_ < 0) return false;
    return startAt(heardIndex_ + 1 < int(queue_.size()) ? heardIndex_ + 1 : 0);
}

bool PlayerController::prev() {
    if (heardIndex_ < 0) return false;
    int i = heardIndex_ - 1;
    if (i < 0) i = 0;
    return startAt(i);
}

bool PlayerController::seekMs(uint64_t ms) {
    if (!cur_.dec || rate_ == 0) return false;
    uint64_t frame = ms * rate_ / 1000;
    if (!cur_.dec->seek(frame)) return false;
    // Cancel any pending gapless preload and restart the ring from the seek point.
    ring_.reset();
    next_ = Loaded{};
    fedIndex_ = heardIndex_;
    pump_ = std::make_unique<DecodePump>(cur_.dec.get(), &ring_, channels_, 512);
    curTrackStartRendered_ = renderedFrames();
    curTrackStartFrame_ = frame;
    boundariesHeard_ = 0;
    preloadNext();
    pump_->fill();
    return true;
}

void PlayerController::preloadNext() {
    if (!pump_ || pump_->hasNext()) return;
    int n = fedIndex_ + 1;
    if (n >= int(queue_.size())) return;
    Loaded l = loadIndex(n);
    if (!l.dec) return;
    // Gapless only when the format matches; otherwise leave it for a reclock on
    // track end (HW reclock path; host stub keeps a single rate).
    if (l.fmt.sampleRate != rate_ || l.fmt.channels != channels_) return;
    next_ = std::move(l);
    pump_->setNext(next_.dec.get());
    fedIndex_ = n;
}

void PlayerController::advanceHeard() {
    if (!pump_) return;
    const auto& bounds = pump_->trackBoundaries();
    uint64_t rendered = renderedFrames();
    // Count boundaries the heard stream has crossed.
    while (boundariesHeard_ < bounds.size() &&
           bounds[boundariesHeard_] <= rendered) {
        curTrackStartRendered_ = bounds[boundariesHeard_];
        curTrackStartFrame_ = 0;
        ++boundariesHeard_;
        if (heardIndex_ + 1 < int(queue_.size()))
            ++heardIndex_;
    }
}

void PlayerController::serviceDecode() {
    if (transport_ == Transport::Stopped || !pump_) return;

    bool hadNext = pump_->hasNext();
    pump_->fill();
    // If the pump consumed its queued next (handoff at the decode boundary),
    // rotate ownership so exactly two decoders stay alive, then refill next.
    if (hadNext && !pump_->hasNext()) {
        cur_ = std::move(next_);
        next_ = Loaded{};
        preloadNext();
    }
    preloadNext();  // also covers the no-next-yet case
    advanceHeard();

    // End of queue: pump drained and everything decoded has been heard.
    if (pump_->eof() && ring_.readable() == 0) {
        const auto& bounds = pump_->trackBoundaries();
        uint64_t produced = pump_->framesProduced();
        if (renderedFrames() >= produced && (bounds.empty() ||
            renderedFrames() >= bounds.back())) {
            transport_ = Transport::Stopped;
        }
    }
}

PlayerState PlayerController::state() const {
    PlayerState s;
    s.transport = transport_;
    s.queueSize = queue_.size();
    s.underrunFrames = engine_.underrunFrames();
    if (heardIndex_ >= 0 && std::size_t(heardIndex_) < queue_.size()) {
        s.queueIndex = heardIndex_;
        s.path = queue_[std::size_t(heardIndex_)];
        s.durationMs = durationMs_[std::size_t(heardIndex_)];
        uint64_t rendered = renderedFrames();
        uint64_t posFrames = rendered >= curTrackStartRendered_
                                 ? rendered - curTrackStartRendered_
                                 : 0;
        posFrames += curTrackStartFrame_;
        s.positionMs = rate_ ? posFrames * 1000 / rate_ : 0;
    }
    return s;
}

}  // namespace carsh::player

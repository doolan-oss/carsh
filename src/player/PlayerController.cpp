#include "player/PlayerController.h"

#include "audio/ChannelMix.h"
#include "decode/DecoderFactory.h"

namespace carsh::player {

using carsh::audio::ChannelMix;
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
    unplayable_.assign(queue_.size(), 0);
}

void PlayerController::clearQueue() {
    stop();
    queue_.clear();
    durationMs_.clear();
    unplayable_.clear();
}

PlayerController::Loaded PlayerController::loadIndex(int index) {
    Loaded l;
    if (index < 0 || std::size_t(index) >= queue_.size()) return l;
    const std::size_t i = std::size_t(index);
    l.stream = vfs_.open(queue_[i]);
    if (l.stream) l.dec = carsh::decode::openAuto(l.stream.get());
    if (l.dec) l.fmt = l.dec->format();
    // Refuse anything the pump can't map onto the output channels (e.g. a >8ch
    // or 0-channel WAV) here, cleanly, rather than let it near the scratch buffer.
    if (!l.dec || !l.fmt.valid() || !ChannelMix::supported(l.fmt.channels)) {
        l.release();
        unplayable_[i] = 1;  // remembered: auto-advance/preload won't retry it
        return l;
    }
    l.index = index;
    unplayable_[i] = 0;  // an explicit retry that works clears the mark
    durationMs_[i] = l.dec->durationMs();
    return l;
}

void PlayerController::newPump() {
    pump_ = std::make_unique<DecodePump>(cur_.dec.get(), &ring_, channels_, 512);
    chained_.clear();
    boundariesHeard_ = 0;
    successorResolved_ = false;
    pumpBaseRendered_ = renderedFrames();
}

bool PlayerController::startAt(int index) {
    Loaded l = loadIndex(index);
    if (!l.dec) return false;
    return startLoaded(std::move(l), /*flushRing=*/true);
}

bool PlayerController::startLoaded(Loaded l, bool flushRing) {
    // Fresh playback context. A user skip flushes whatever is still buffered; an
    // automatic restart happens only once the ring has fully drained, so it
    // leaves the ring alone (the DMA callback may be reading it right now).
    if (flushRing) ring_.reset();
    engine_.resetRenderedCount();
    engine_.resetUnderrunCount();
    engine_.setPaused(false);

    pump_.reset();  // the old pump goes first: it points at the decoders below
    next_.release();
    staged_.release();
    cur_ = std::move(l);
    rate_ = cur_.fmt.sampleRate;
    newPump();

    heardIndex_ = cur_.index;
    curTrackStartRendered_ = pumpBaseRendered_;
    curTrackStartFrame_ = 0;
    transport_ = Transport::Playing;
    preloadNext();
    topUp();
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
    cur_.release();
    next_.release();
    staged_.release();
    chained_.clear();
    ring_.reset();
    heardIndex_ = -1;
}

bool PlayerController::next() {
    if (heardIndex_ < 0 || queue_.empty()) return false;
    const int n = int(queue_.size());
    // Forward (wrapping) to the first entry that opens; the last candidate is
    // the current track itself (a one-track queue restarts it, as before).
    for (int k = 1; k <= n; ++k) {
        const int i = (heardIndex_ + k) % n;
        if (i != heardIndex_ && knownUnplayable(i)) continue;
        if (startAt(i)) return true;
    }
    return false;
}

bool PlayerController::prev() {
    if (heardIndex_ < 0) return false;
    for (int i = heardIndex_ - 1; i >= 0; --i) {
        if (knownUnplayable(i)) continue;
        if (startAt(i)) return true;
    }
    return startAt(heardIndex_);  // nothing playable before: restart this one
}

bool PlayerController::seekMs(uint64_t ms) {
    if (!pump_ || !cur_.dec || heardIndex_ < 0 || rate_ == 0) return false;
    const uint64_t frame = ms * rate_ / 1000;
    Loaded reopened;
    if (cur_.index == heardIndex_) {
        if (!cur_.dec->seek(frame)) return false;
    } else {
        // The pump has already moved on to the next track (the heard track's
        // tail is still in the ring) — reopen the heard track to seek in it.
        reopened = loadIndex(heardIndex_);
        if (!reopened.dec || !reopened.dec->seek(frame)) return false;
    }
    // Cancel any pending gapless preload and restart the ring from the seek point.
    ring_.reset();
    pump_.reset();
    next_.release();
    staged_.release();
    if (reopened.dec) {
        cur_ = std::move(reopened);
        rate_ = cur_.fmt.sampleRate;
    }
    newPump();
    curTrackStartRendered_ = pumpBaseRendered_;
    curTrackStartFrame_ = frame;
    preloadNext();
    topUp();
    return true;
}

void PlayerController::syncHandoff() {
    // The pump moved onto the queued decoder: it is the current one now, and the
    // old current (drained, no longer referenced by the pump) can be released.
    if (pump_ && next_.dec && pump_->current() == next_.dec.get()) {
        cur_ = std::move(next_);
        next_.release();
        successorResolved_ = false;
    }
}

void PlayerController::preloadNext() {
    if (!pump_) return;
    syncHandoff();  // never overwrite next_ while the pump is still reading it
    if (successorResolved_ || pump_->hasNext() || pump_->eof()) return;
    successorResolved_ = true;  // decided once per current track, not per slice
    for (int n = cur_.index + 1; n < int(queue_.size()); ++n) {
        if (knownUnplayable(n)) continue;
        Loaded l = loadIndex(n);
        if (!l.dec) continue;  // now marked; try the entry after it
        if (l.fmt.sampleRate == rate_) {
            next_ = std::move(l);
            chained_.push_back(n);
            pump_->setNext(next_.dec.get());
        } else {
            // Different rate: can't share the ring/clock. Hold it (opened once)
            // and restart on it non-gapless when the current track has played out.
            staged_ = std::move(l);
        }
        return;
    }
}

void PlayerController::topUp() {
    if (!pump_) return;
    // fill() stops at each track boundary; rotate ownership and chain the track
    // after before filling on, so a short track that drains mid-fill never leaves
    // the pump reading a decoder we're about to replace. Bounded: each extra pass
    // crosses one boundary, and there are at most queue-size of them.
    for (std::size_t pass = 0; pass <= queue_.size(); ++pass) {
        const std::size_t done = pump_->tracksCompleted();
        pump_->fill();
        syncHandoff();
        preloadNext();
        if (pump_->tracksCompleted() == done || pump_->eof()) break;
    }
}

void PlayerController::advanceHeard() {
    if (!pump_) return;
    const auto& bounds = pump_->trackBoundaries();
    const uint64_t heard = renderedByPump();
    // Each boundary is a real handoff; chained_ says which track it led to.
    while (boundariesHeard_ < bounds.size() &&
           bounds[boundariesHeard_] <= heard) {
        curTrackStartRendered_ = pumpBaseRendered_ + bounds[boundariesHeard_];
        curTrackStartFrame_ = 0;
        if (boundariesHeard_ < chained_.size())
            heardIndex_ = chained_[boundariesHeard_];
        ++boundariesHeard_;
    }
}

bool PlayerController::restartOnSuccessor() {
    // Prefer the successor preloadNext already opened (rate mismatch); else find
    // the next entry that opens (preload may not have run, or found nothing).
    Loaded l = std::move(staged_);
    staged_.release();
    for (int n = cur_.index + 1; !l.dec && n < int(queue_.size()); ++n) {
        if (knownUnplayable(n)) continue;
        l = loadIndex(n);
    }
    if (!l.dec) return false;
    return startLoaded(std::move(l), /*flushRing=*/false);
}

void PlayerController::serviceDecode() {
    if (transport_ == Transport::Stopped || !pump_) return;

    topUp();
    advanceHeard();

    // Current source drained and everything it produced has been heard. Only
    // while Playing: a pause that lands just as the ring runs dry is kept, and
    // the advance happens on resume.
    if (transport_ == Transport::Playing && pump_->eof() &&
        ring_.readable() == 0 && renderedByPump() >= pump_->framesProduced()) {
        // No gapless successor was chained (rate change, or nothing after it):
        // continue non-gapless on the next playable track, else end of queue.
        if (!restartOnSuccessor()) transport_ = Transport::Stopped;
    }
}

bool PlayerController::ownershipConsistent() const {
    if (!pump_) return true;  // nothing is being read
    return pump_->current() == cur_.dec.get() &&
           pump_->queuedNext() == next_.dec.get();
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
        s.sampleRate = rate_;
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

// DecodePump — the decode task's ring-filling logic (PLAN.md §1.2/§5.1/§5.4).
//
// The sole producer for the PcmRing. It decodes in blocks of <= ~10 ms and, on
// the real device, yields after each block so it never monopolizes the
// cooperative scheduler. Here the logic is exposed as pumpOnce()/fill() so host
// tests can drive it deterministically and interleave it with device pumps.
//
// Channels: the constructor's `channels` is the OUTPUT (ring/engine) count. Each
// decoder writes frames in its OWN count (IDecoder contract), so the scratch
// buffer is sized from the decoder's format() — re-checked before every decode()
// so no decoder can write past it — and each block goes through a ChannelMix
// (mono upmix / multichannel downmix) before it enters the ring. A decoder whose
// channel count ChannelMix can't map is treated as already drained.
//
// Gapless: when the current decoder reaches EOF and a `next` decoder is queued,
// the pump switches to it and keeps appending to the SAME ring with no silence
// (PLAN.md §5.4) — provided the sample rates match (else the controller restarts
// non-gapless and the device reclocks). Channel counts may differ: both are
// mixed to the output count. An optional leading-frame skip strips encoder
// delay/padding.
//
// fill() returns at every track boundary so the owner can take ownership of the
// decoder the pump just moved onto and queue the one after it before decoding
// continues (see PlayerController::topUp()).
#pragma once

#include <cstddef>
#include <vector>

#include "audio/ChannelMix.h"
#include "audio/PcmRing.h"
#include "decode/IDecoder.h"

namespace carsh::audio {

class DecodePump {
public:
    DecodePump(carsh::decode::IDecoder* dec, PcmRing<float>* ring,
               uint8_t channels, std::size_t blockFrames = 512)
        : dec_(dec),
          ring_(ring),
          channels_(channels),
          blockFrames_(blockFrames),
          mixed_(blockFrames * channels) {
        reserveFor(dec_);
    }

    // Queue the next track for gapless handoff (same ring). Optionally skip the
    // first `skipFrames` of it (encoder delay).
    void setNext(carsh::decode::IDecoder* next, std::size_t skipFrames = 0) {
        next_ = next;
        nextSkip_ = skipFrames;
        reserveFor(next_);  // allocate now, not at the seam
    }

    // Skip the leading `n` decoded frames of the CURRENT decoder (encoder delay).
    void setLeadingSkip(std::size_t n) { skip_ = n; }

    // One cooperative slice: decode up to one block into the ring (bounded by
    // ring space). Returns true if it wrote samples or could write more later;
    // false only when fully drained (current+next EOF) so the caller can stop.
    bool pumpOnce() {
        if (eof_) return false;
        std::size_t roomFrames = ring_->writable() / channels_;
        if (roomFrames == 0) return true;  // ring full; try again later
        std::size_t want = roomFrames < blockFrames_ ? roomFrames : blockFrames_;

        // The decoder's own channel count, read per block (a streaming decoder may
        // revise it) so scratch always holds want * inCh floats.
        const uint8_t inCh = dec_ ? dec_->format().channels : 0;
        std::size_t got = 0;
        if (ChannelMix::supported(inCh)) {
            if (scratch_.size() < want * inCh) scratch_.resize(want * inCh);
            got = dec_->decode(scratch_.data(), want);
            if (got > want) got = want;  // never trust a decoder past its buffer
        }
        if (got == 0) {
            // Current decoder drained — hand off to next, or finish.
            ++tracksCompleted_;
            if (next_) {
                // Record cumulative output frames at this handoff. Because nothing
                // written to the ring is ever dropped, this equals the output-frame
                // index at which the heard stream crosses into the next track — the
                // controller maps it onto rendered frames for the now-playing
                // index. Only real successors get a boundary.
                boundaries_.push_back(framesProduced_);
                dec_ = next_;
                next_ = nullptr;
                skip_ = nextSkip_;
                nextSkip_ = 0;
                return true;  // resume next slice on the new decoder
            }
            eof_ = true;
            return false;
        }

        std::size_t off = 0;
        if (skip_ > 0) {
            std::size_t s = skip_ < got ? skip_ : got;
            off = s;
            skip_ -= s;
            got -= s;
        }
        if (got > 0) {
            const float* src = scratch_.data() + off * inCh;
            if (inCh != channels_) {
                if (mix_.in() != inCh || mix_.out() != channels_)
                    mix_.configure(inCh, channels_);
                mix_.apply(src, mixed_.data(), got);
                src = mixed_.data();
            }
            std::size_t wrote = ring_->write(src, got * channels_);
            framesProduced_ += wrote / channels_;
        }
        return true;
    }

    // Fill the ring as far as possible right now (prime / top-up to high-water).
    // Stops early at a track boundary (see header note) — call again to go on.
    // Returns false when the source is fully drained.
    bool fill() {
        bool alive = true;
        const std::size_t done = tracksCompleted_;
        while (ring_->writable() >= channels_ && !eof_) {
            std::size_t before = ring_->writable();
            alive = pumpOnce();
            if (!alive) break;
            if (tracksCompleted_ != done) break;     // boundary: let the owner rotate
            if (ring_->writable() == before) break;  // no progress (would spin)
        }
        return alive;
    }

    bool eof() const { return eof_; }
    std::size_t framesProduced() const { return framesProduced_; }
    // Number of decoders that have fully drained (track ends crossed).
    std::size_t tracksCompleted() const { return tracksCompleted_; }
    // Cumulative output-frame index at each gapless handoff (where the queued
    // next track starts). A final drain with no successor adds no entry.
    const std::vector<std::size_t>& trackBoundaries() const { return boundaries_; }
    // True until the queued `next` has been promoted to current.
    bool hasNext() const { return next_ != nullptr; }
    // The decoder being read now / the one queued behind it (ownership checks).
    const carsh::decode::IDecoder* current() const { return dec_; }
    const carsh::decode::IDecoder* queuedNext() const { return next_; }
    // Scratch capacity in floats (tests assert decode() never exceeds it).
    std::size_t scratchSize() const { return scratch_.size(); }
    // Replace the current decoder (used by the controller on seek/skip).
    void setCurrent(carsh::decode::IDecoder* dec) {
        dec_ = dec;
        eof_ = false;
        skip_ = 0;
        reserveFor(dec_);
    }

private:
    void reserveFor(const carsh::decode::IDecoder* d) {
        if (!d) return;
        const uint8_t ch = d->format().channels;
        if (ChannelMix::supported(ch) && scratch_.size() < blockFrames_ * ch)
            scratch_.resize(blockFrames_ * ch);
    }

    carsh::decode::IDecoder* dec_;
    carsh::decode::IDecoder* next_ = nullptr;
    PcmRing<float>* ring_;
    uint8_t channels_;
    std::size_t blockFrames_;
    std::vector<float> scratch_;  // decoder-channel frames
    std::vector<float> mixed_;    // output-channel frames (when counts differ)
    ChannelMix mix_;
    std::size_t skip_ = 0;
    std::size_t nextSkip_ = 0;
    std::size_t framesProduced_ = 0;
    std::size_t tracksCompleted_ = 0;
    std::vector<std::size_t> boundaries_;
    bool eof_ = false;
};

}  // namespace carsh::audio

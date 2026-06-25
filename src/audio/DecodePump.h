// DecodePump — the decode task's ring-filling logic (PLAN.md §1.2/§5.1/§5.4).
//
// The sole producer for the PcmRing. It decodes in blocks of <= ~10 ms and, on
// the real device, yields after each block so it never monopolizes the
// cooperative scheduler. Here the logic is exposed as pumpOnce()/fill() so host
// tests can drive it deterministically and interleave it with device pumps.
//
// Gapless: when the current decoder reaches EOF and a `next` decoder is queued,
// the pump switches to it and keeps appending to the SAME ring with no silence
// (PLAN.md §5.4) — provided the formats match (else the controller reclocks).
// An optional leading-frame skip strips encoder delay/padding.
#pragma once

#include <cstddef>
#include <vector>

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
          scratch_(blockFrames * channels) {}

    // Queue the next track for gapless handoff (same ring). Optionally skip the
    // first `skipFrames` of it (encoder delay).
    void setNext(carsh::decode::IDecoder* next, std::size_t skipFrames = 0) {
        next_ = next;
        nextSkip_ = skipFrames;
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

        std::size_t got = dec_->decode(scratch_.data(), want);
        if (got == 0) {
            // Current decoder drained — hand off to next, or finish.
            ++tracksCompleted_;
            // Record cumulative output frames at this boundary. Because nothing
            // written to the ring is ever dropped, this equals the output-frame
            // index at which the heard stream crosses into the next track — the
            // controller maps it onto rendered frames for the now-playing index.
            boundaries_.push_back(framesProduced_);
            if (next_) {
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
            std::size_t wrote =
                ring_->write(scratch_.data() + off * channels_, got * channels_);
            framesProduced_ += wrote / channels_;
        }
        return true;
    }

    // Fill the ring as far as possible right now (prime / top-up to high-water).
    // Returns false when the source is fully drained.
    bool fill() {
        bool alive = true;
        while (ring_->writable() >= channels_ && !eof_) {
            std::size_t before = ring_->writable();
            alive = pumpOnce();
            if (!alive) break;
            if (ring_->writable() == before) break;  // no progress (would spin)
        }
        return alive;
    }

    bool eof() const { return eof_; }
    std::size_t framesProduced() const { return framesProduced_; }
    // Number of decoders that have fully drained (track boundaries crossed).
    std::size_t tracksCompleted() const { return tracksCompleted_; }
    // Cumulative output-frame index at each completed track boundary.
    const std::vector<std::size_t>& trackBoundaries() const { return boundaries_; }
    // True once the queued `next` has been promoted to current.
    bool hasNext() const { return next_ != nullptr; }
    // Replace the current decoder (used by the controller on seek/skip).
    void setCurrent(carsh::decode::IDecoder* dec) {
        dec_ = dec;
        eof_ = false;
        skip_ = 0;
    }

private:
    carsh::decode::IDecoder* dec_;
    carsh::decode::IDecoder* next_ = nullptr;
    PcmRing<float>* ring_;
    uint8_t channels_;
    std::size_t blockFrames_;
    std::vector<float> scratch_;
    std::size_t skip_ = 0;
    std::size_t nextSkip_ = 0;
    std::size_t framesProduced_ = 0;
    std::size_t tracksCompleted_ = 0;
    std::vector<std::size_t> boundaries_;
    bool eof_ = false;
};

}  // namespace carsh::audio

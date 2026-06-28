// AsyncH264Decoder — an IH264Decoder that DECOUPLES the AA receive path from the
// (heavy, synchronous) H.264 decode (AA_AUDIO_INVESTIGATION.md fix #3).
//
// It wraps the real decoder (device: CircleH264Decoder/openh264; host test: a fake)
// and an AuQueue. feed() — called from the AA poll loop — only copies the access
// unit into the queue and returns, so that loop keeps draining AA audio into
// g_aaRing promptly instead of stalling on a keyframe decode. A separate consumer
// (a cooperative decode task today; a dedicated A72 core via CMultiCoreSupport
// later) calls pump() to actually run the inner decoder on its own schedule.
//
// Threading contract: feed() is the SOLE producer (AA task); pump() is the SOLE
// consumer (decode task/core) — matching AuQueue's SPSC requirement. The inner
// decoder, its frame sink, and everything the sink touches therefore run on the
// PUMP side. With a cooperative pump task (same core) the sink's setFrame() and the
// UI render() never overlap, so no extra synchronisation is needed; moving pump()
// to a real second core additionally requires a double-buffered frame handoff (see
// device/CircleAaDecodeCore.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "aa/AuQueue.h"
#include "aa/IH264Decoder.h"

namespace carsh::aa {

class AsyncH264Decoder : public IH264Decoder {
public:
    // inner: the real decoder, run on the pump side (may be nullptr until wired).
    // queueBytes: AuQueue capacity. A few keyframes of headroom (the device uses
    // ~64 KB AUs) lets a brief decode stall ride out without dropping AUs.
    AsyncH264Decoder(IH264Decoder* inner, std::size_t queueBytes)
        : queue_(queueBytes), inner_(inner) {}

    // Producer (AA poll loop): enqueue the AU; never decodes here. A drop (queue
    // full) is counted in the AuQueue and surfaces in the decode-health log.
    void feed(const std::uint8_t* annexB, std::size_t len) override {
        if (annexB == nullptr || len == 0) return;
        queue_.push(annexB, len);
    }

    // Consumer (decode task/core): decode up to maxAus queued AUs, returning how
    // many were processed (0 if the queue was empty). When decoding is disabled
    // (the A/B instrumentation toggle, step 3) AUs are popped but NOT decoded, so
    // the queue still drains and we can listen for whether audio smooths out —
    // confirming decode-on-the-refill-thread as the choppiness cause.
    unsigned pump(unsigned maxAus = 8) {
        unsigned n = 0;
        while (n < maxAus && queue_.pop(scratch_)) {
            if (decode_ && inner_ != nullptr && !scratch_.empty())
                inner_->feed(scratch_.data(), scratch_.size());
            ++n;
        }
        return n;
    }

    void setDecodeEnabled(bool on) { decode_ = on; }
    bool decodeEnabled() const { return decode_; }

    const AuQueue& queue() const { return queue_; }

private:
    AuQueue queue_;
    IH264Decoder* inner_;
    bool decode_ = true;
    std::vector<std::uint8_t> scratch_;  // reused AU buffer (pump side only)
};

}  // namespace carsh::aa

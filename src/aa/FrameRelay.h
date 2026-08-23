// FrameRelay — lock-free single-producer / single-consumer handoff of the latest
// decoded RGB frame from the AA decode core to the UI core (the cross-core seam the
// dedicated-decode-core needs; see device/CircleAaDecodeCore.h).
//
// Triple-buffered: the producer always writes a slot the consumer is NOT reading,
// and the consumer always reads a fully-written slot, so a frame is never torn.
// Stale frames are dropped (latest-wins — the UI only ever wants the newest picture).
//
// Correctness: the three slot indices held by {producer-back, consumer-read, shared}
// are always a permutation of {0,1,2} (each swap exchanges exactly two of them), so
// producer-back != consumer-read at all times. The single shared word is updated by
// an atomic exchange with acquire/release ordering, which (on the cache-coherent,
// inner-shareable A72 cluster that ARM_ALLOW_MULTI_CORE configures) makes the
// producer's frame writes visible to the consumer before it reads them. Same lock-
// free discipline as AuQueue; no cache maintenance needed on coherent memory.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace carsh::aa {

class FrameRelay {
public:
    FrameRelay() { state_.store(pack(2, false), std::memory_order_relaxed); }

    // --- Producer (decode core) -------------------------------------------
    // Copy a w*h*3 RGB frame into the back buffer and publish it (latest-wins).
    void publish(const std::uint8_t* rgb, int w, int h) {
        if (rgb == nullptr || w <= 0 || h <= 0) return;
        Slot& s = slot_[back_];
        const std::size_t n = std::size_t(w) * h * 3;
        s.rgb.assign(rgb, rgb + n);   // into our exclusively-owned back buffer
        s.w = w;
        s.h = h;
        // Swap our freshly-written slot into the shared cell; adopt whatever was
        // there as the new back. acq_rel so the buffer writes publish before the swap.
        const unsigned prev = state_.exchange(pack(back_, true), std::memory_order_acq_rel);
        back_ = idx(prev);
        published_.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Consumer (UI core) -----------------------------------------------
    // If a newer frame exists, swap it in and hand it back (the returned pointer is
    // valid until the next acquire()). Returns false if nothing new since last call.
    bool acquire(const std::uint8_t*& rgb, int& w, int& h) {
        if (!fresh(state_.load(std::memory_order_acquire))) return false;
        const unsigned prev = state_.exchange(pack(read_, false), std::memory_order_acq_rel);
        read_ = idx(prev);
        const Slot& s = slot_[read_];
        rgb = s.rgb.data();
        w = s.w;
        h = s.h;
        consumed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::uint64_t published() const { return published_.load(std::memory_order_relaxed); }
    std::uint64_t consumed() const { return consumed_.load(std::memory_order_relaxed); }

private:
    struct Slot { std::vector<std::uint8_t> rgb; int w = 0, h = 0; };

    // shared word: slot index in the low 2 bits, "fresh" flag in bit 2.
    static unsigned pack(unsigned i, bool f) { return (i & 0x3u) | (f ? 0x4u : 0u); }
    static unsigned idx(unsigned st) { return st & 0x3u; }
    static bool fresh(unsigned st) { return (st & 0x4u) != 0; }

    Slot slot_[3];
    unsigned back_ = 0;   // producer-owned (never read by the consumer)
    unsigned read_ = 1;   // consumer-owned (never written by the producer)
    alignas(64) std::atomic<unsigned> state_{0};  // shared (starts at slot 2)
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> consumed_{0};
};

}  // namespace carsh::aa

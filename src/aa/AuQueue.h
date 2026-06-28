// AuQueue — a framed single-producer/single-consumer byte ring for handing encoded
// H.264 access units (AUs) from the Android Auto receive path to a separate decoder
// (AA_AUDIO_INVESTIGATION.md fix #3: get the synchronous openh264 decode OFF the
// thread that refills the audio ring).
//
// Today AaProjection::onAvMedia calls decoder->feed() inline inside the AA poll
// loop, so a long keyframe decode blocks the same loop that drains AA audio into
// g_aaRing -> the PWM consumer underruns. AuQueue is the seam: the AA task pushes
// the AU bytes (cheap copy) and returns immediately; a decode task/core pops whole
// AUs and runs openh264 on its own schedule (a cooperative task now; a dedicated
// A72 core via CMultiCoreSupport later — see device/CircleAaDecodeCore.h).
//
// Framing: each AU is stored as [u32 little-endian length][payload]. The producer
// writes both into the buffer at relaxed positions, then publishes the new head
// with ONE release store, so the consumer never observes a half-written frame (the
// reason this isn't built on PcmRing, whose per-call publish can't span the
// length+payload pair). The consumer reads the header then the payload under the
// framing invariant that a published AU is wholly present. Same wait-free, no-lock
// discipline as PcmRing; capacity rounded up to a power of two for mask wrap.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace carsh::aa {

class AuQueue {
public:
    // requestedCapacity is in bytes (total framing overhead + payloads it can hold
    // at once). Usable capacity is the next power of two >= requested, minus one.
    explicit AuQueue(std::size_t requestedCapacity) {
        std::size_t cap = 2;
        while (cap < requestedCapacity + 1) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    std::size_t capacity() const { return mask_; }

    // --- Producer side (AA receive task) -----------------------------------

    // Enqueue one access unit. Returns false (and counts a drop) if it doesn't fit
    // right now — the caller's stream is bursting faster than the decoder drains, so
    // we drop a whole AU rather than corrupt the framing. Never blocks.
    bool push(const std::uint8_t* au, std::size_t len) {
        if (len > 0xFFFFFFFFu) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
        const std::size_t need = kHeader + len;
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t free = (tail + capacity() - head) & mask_;
        if (free < need) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::size_t p = head;
        const std::uint32_t l = static_cast<std::uint32_t>(len);
        for (int i = 0; i < 4; ++i) { buf_[p & mask_] = std::uint8_t(l >> (8 * i)); p = (p + 1) & mask_; }
        for (std::size_t i = 0; i < len; ++i) { buf_[p & mask_] = au[i]; p = (p + 1) & mask_; }
        head_.store(p, std::memory_order_release);  // single publish for the whole AU
        pushed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // --- Consumer side (decode task / core) --------------------------------

    // Pop the next AU into `out` (resized to the AU length). Returns false if none
    // is queued. Never blocks.
    bool pop(std::vector<std::uint8_t>& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t avail = (head - tail) & mask_;
        if (avail < kHeader) return false;
        std::size_t p = tail;
        std::uint32_t l = 0;
        for (int i = 0; i < 4; ++i) { l |= std::uint32_t(buf_[p & mask_]) << (8 * i); p = (p + 1) & mask_; }
        // Framing invariant: head was published only after the full AU was written,
        // so the payload is wholly present here.
        out.resize(l);
        for (std::size_t i = 0; i < l; ++i) { out[i] = buf_[p & mask_]; p = (p + 1) & mask_; }
        tail_.store(p, std::memory_order_release);
        popped_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // --- Diagnostics (any thread) ------------------------------------------
    std::uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    std::uint64_t popped() const { return popped_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Bytes currently queued (framing + payloads), consumer/producer-consistent.
    std::size_t pending() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & mask_;
    }

private:
    static constexpr std::size_t kHeader = 4;  // u32 LE length prefix

    std::vector<std::uint8_t> buf_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::size_t> head_{0};  // next write index
    alignas(64) std::atomic<std::size_t> tail_{0};  // next read index
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> popped_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace carsh::aa

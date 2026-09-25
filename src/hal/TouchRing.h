// TouchRing — a fixed-capacity, allocation-free SPSC queue for raw touch events,
// the hand-off between the touch driver pump and the UI loop.
//
// Why it exists: the Pi touch firmware keeps ONE snapshot of the contacts, and
// CTouchScreenDevice::Update() only diffs the latest snapshot against the last one
// it saw. Anything that lives only between two Update() calls (a short tap, the
// middle of a flick) is never seen, so the driver must be pumped at >= 60 Hz, and
// the UI loop (one iteration per rendered frame, 45-100 ms) is far too slow. The
// device pumps Update() from Circle's 100 Hz timer IRQ instead (CircleTouch), and
// the events cross to the UI task through this ring.
//
// Contract, so it is safe in IRQ context: one producer (the timer IRQ), one
// consumer (the UI task), no locks, no allocation, no logging. Each side advances
// only its own index and publishes it with a release store; the other side reads
// it with an acquire load. Indices are free-running 32-bit counters (N divides
// 2^32, so wrap-around is harmless) and N is a power of two so the slot is a mask.
//
// Backlog policy: Moves are lossy, transitions are not. Once the ring is nearly
// full (fewer than kReserve free slots) further Moves are dropped — a later Move or
// the Up supersedes them — so the reserved slots stay free for the Down/Up events
// that start and end a contact. Only a completely full ring drops a Down/Up. Drops
// are counted for the diagnostics log, and a lost Down/Up is also counted on its
// own: a consumer that tracks contacts (ScreenManager's primary-contact latch) may
// be left waiting for an Up that never comes, so it checks
// lostTransitionSinceLastCheck() after each drain and resets its contact state.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hal/Touch.h"

namespace carsh::hal {

// A raw touch event plus its capture time on a monotonic 64-bit microsecond clock
// (the device stamps it in the driver pump, so a UI-loop backlog doesn't bunch the
// timestamps the AA phone uses for fling velocity).
struct StampedTouch {
    TouchEvent ev;
    std::uint64_t us = 0;
};

// Generic lock-free single-producer/single-consumer ring of trivially copyable T.
template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "SpscRing capacity must be a power of two");
    static_assert(N <= (std::size_t(1) << 31), "SpscRing capacity too large");

public:
    static constexpr std::size_t capacity() { return N; }

    // Producer only. False (and counted in dropped()) if the ring is full.
    bool push(const T& v) {
        const std::uint32_t h = head_.load(std::memory_order_relaxed);
        const std::uint32_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= N) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. False if empty.
    bool pop(T& out) {
        const std::uint32_t t = tail_.load(std::memory_order_relaxed);
        const std::uint32_t h = head_.load(std::memory_order_acquire);
        if (h == t) return false;
        out = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Either side (a snapshot; exact on the calling side's own index).
    std::size_t size() const {
        return std::size_t(head_.load(std::memory_order_acquire) -
                           tail_.load(std::memory_order_acquire));
    }
    bool empty() const { return size() == 0; }

    // Events refused so far (any thread; diagnostics only).
    std::uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

protected:
    void countDrop() { dropped_.fetch_add(1, std::memory_order_relaxed); }

private:
    std::array<T, N> buf_{};
    std::atomic<std::uint32_t> head_{0};  // written by the producer only
    std::atomic<std::uint32_t> tail_{0};  // written by the consumer only
    std::atomic<std::uint32_t> dropped_{0};
};

// The touch queue: 128 events (32 bytes each, ~4 KiB) is > 1 s of a single
// finger's Moves at the 100 Hz pump rate, far more than one UI frame's worth.
class TouchRing : public SpscRing<StampedTouch, 128> {
public:
    // Free slots kept for Down/Up once the ring is backed up (see the header).
    static constexpr std::size_t kReserve = 16;

    // Producer only (the driver pump). Applies the backlog policy.
    bool pushEvent(const StampedTouch& e) {
        if (e.ev.phase == TouchPhase::Move) {
            if (size() >= capacity() - kReserve) {
                countDrop();
                return false;
            }
            return push(e);
        }
        if (push(e)) return true;
        // A Down/Up lost to a completely full ring (push() counted it in dropped()).
        droppedTransitions_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Down/Up events refused so far (any thread; a subset of dropped()).
    std::uint32_t droppedTransitions() const {
        return droppedTransitions_.load(std::memory_order_relaxed);
    }

    // Consumer only: true if a Down/Up has been dropped since the previous call.
    // Call it after draining: every event queued before the drop was valid and has
    // been delivered, so the consumer can then reset per-contact state that may be
    // waiting for the lost Up.
    bool lostTransitionSinceLastCheck() {
        const std::uint32_t n = droppedTransitions();
        if (n == seenTransitionDrops_) return false;
        seenTransitionDrops_ = n;
        return true;
    }

private:
    std::atomic<std::uint32_t> droppedTransitions_{0};  // producer writes
    std::uint32_t seenTransitionDrops_ = 0;              // consumer only
};

}  // namespace carsh::hal

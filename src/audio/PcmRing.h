// PcmRing — lock-free single-producer/single-consumer ring buffer.
//
// This is the seam between the decode task (sole producer) and the I2S DMA pull
// callback (sole consumer), per PLAN.md §1.2/§5.1. Correctness requirements:
//   * No locks. The consumer runs in IRQ/DMA context and must never block on a
//     slow producer.
//   * Wait-free push/pop. Each side only advances its own index and only reads
//     the other's index; no CAS loops.
//   * Sized to hold ~500 ms–1 s of audio so an SD/USB/UI stall never starves
//     the DMA.
//
// Capacity is rounded up to a power of two so index wrap is a mask, not a
// modulo. One slot is left unused to distinguish full from empty without a
// separate count, which keeps the two indices independent (the classic SPSC
// trick that makes the memory ordering sound).
//
// Memory ordering: the producer publishes data with a release store to head_;
// the consumer observes it with an acquire load, and vice-versa for tail_. That
// pair establishes the happens-before edge so written samples are visible before
// the index that exposes them.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace carsh::audio {

// T must be trivially copyable (we memcpy-equivalent via element copy). Intended
// use is T = float for interleaved PCM samples, but kept generic for tests.
template <typename T>
class PcmRing {
public:
    // requestedCapacity is in elements (samples, not frames). The usable
    // capacity is the next power of two >= requestedCapacity, minus one slot.
    explicit PcmRing(std::size_t requestedCapacity) {
        std::size_t cap = 2;
        while (cap < requestedCapacity + 1) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    // Total elements that can be held at once (one slot reserved).
    std::size_t capacity() const { return mask_; }

    // --- Producer side (decode task) ---------------------------------------

    // Copies up to n elements from src into the ring. Returns the number
    // actually written (< n if the ring filled). Never blocks.
    std::size_t write(const T* src, std::size_t n) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t free = (tail + capacity() - head) & mask_;
        const std::size_t toWrite = n < free ? n : free;
        for (std::size_t i = 0; i < toWrite; ++i)
            buf_[(head + i) & mask_] = src[i];
        head_.store((head + toWrite) & mask_, std::memory_order_release);
        return toWrite;
    }

    // Elements free for writing right now (producer's view).
    std::size_t writable() const {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (tail + capacity() - head) & mask_;
    }

    // --- Consumer side (I2S callback) --------------------------------------

    // Copies up to n elements into dst. Returns the number actually read
    // (< n on underrun). Never blocks. The caller is responsible for zero-
    // filling the remainder on a short read (the engine does this on underrun).
    std::size_t read(T* dst, std::size_t n) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t avail = (head - tail) & mask_;
        const std::size_t toRead = n < avail ? n : avail;
        for (std::size_t i = 0; i < toRead; ++i)
            dst[i] = buf_[(tail + i) & mask_];
        tail_.store((tail + toRead) & mask_, std::memory_order_release);
        return toRead;
    }

    // Discards up to n elements without copying them out (used to drop encoder
    // delay/padding for gapless, and to flush on seek/track-change).
    std::size_t discard(std::size_t n) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t avail = (head - tail) & mask_;
        const std::size_t toDrop = n < avail ? n : avail;
        tail_.store((tail + toDrop) & mask_, std::memory_order_release);
        return toDrop;
    }

    // Elements available to read right now (consumer's view).
    std::size_t readable() const {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return (head - tail) & mask_;
    }

    // Resets both indices. NOT concurrency-safe — only call when neither side is
    // running (e.g. between tracks while the engine is muted/stopped).
    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<T> buf_;
    std::size_t mask_ = 0;
    // Cache-line pad the two indices so the producer and consumer don't ping-
    // pong the same line. 64 bytes is the A72 line size.
    alignas(64) std::atomic<std::size_t> head_{0};  // next write index
    alignas(64) std::atomic<std::size_t> tail_{0};  // next read index
};

}  // namespace carsh::audio

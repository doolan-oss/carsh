// AaBulkLink — the platform-independent half of the Android Auto USB transport
// (device/CircleAoaTransport): the IRQ->task RX byte ring plus a sticky "link dead"
// latch. Host-tested (tests/aa/AaBulkLink_test.cpp); the Circle glue only turns
// bulk-IN URB completions into onInComplete() and blocking bulk-OUT results into
// onOut*().
//
// Why a latch and not a retry (review A1/A3/A7): on the Pi 4 xHCI a bulk transfer
// error cannot be recovered in place. A Transaction/Babble/Stall error halts the
// endpoint; Circle (Step51) completes only the failed URB — the second posted one
// never completes on the halted ring — and never resets the endpoint; a bulk-OUT that
// hits Circle's 3 s timeout leaves its TRB live on the ring. Any byte lost on either
// pipe also breaks the TLS record sequence, so the AA session is over either way.
// So the FIRST fault (failed IN transfer, failed re-post, RX overflow, failed or
// timed-out OUT write) marks the link dead for good: the IN side stops re-posting,
// read() and write() refuse, and the driver (AaLinkSupervisor) ends the session and
// re-enumerates the dongle — a fresh transport object is a fresh link.
//
// Threading: one producer (the bulk-IN completion — USB IRQ context on core 0) and
// one consumer (the UI/AA task). The producer side never allocates, never logs and
// never blocks, so it is IRQ-safe. Acquire/release atomics publish the ring bytes —
// the same SPSC discipline as AuQueue / FrameRelay.
#pragma once

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace carsh::aa {

// Why the link died. The first fault wins; it is sticky for the transport's lifetime.
enum class AaLinkFault : uint8_t {
    None = 0,
    InError,        // a bulk-IN transfer completed with an error (the xHC halts the EP)
    InRearmFailed,  // a bulk-IN request could not be (re-)posted
    RxOverflow,     // a completed bulk-IN transfer did not fit in the RX ring
    OutError,       // a bulk-OUT write failed or timed out
    OutStalled,     // a bulk-OUT write "succeeded" with 0 bytes (can't make progress)
};

inline const char* aaLinkFaultName(AaLinkFault f) {
    switch (f) {
        case AaLinkFault::None: return "none";
        case AaLinkFault::InError: return "bulk-IN transfer error";
        case AaLinkFault::InRearmFailed: return "bulk-IN re-post failed";
        case AaLinkFault::RxOverflow: return "RX ring overflow";
        case AaLinkFault::OutError: return "bulk-OUT failed/timed out";
        case AaLinkFault::OutStalled: return "bulk-OUT made no progress";
    }
    return "?";
}

template <size_t RingSize>
class AaBulkLink {
    static_assert(RingSize >= 2 && (RingSize & (RingSize - 1)) == 0,
                  "RingSize must be a power of two");

public:
    // One slot stays empty so head==tail always means "empty".
    static constexpr size_t kCapacity = RingSize - 1;

    // --- Producer: the bulk-IN completion (USB IRQ context) ------------------
    // ok=false means the transfer failed: data/n are NOT looked at (on Circle's xHCI
    // a failed URB has no valid length — reading it asserts inside the ISR, A1).
    // A transfer is taken whole or not at all: if it doesn't fit, the link dies
    // (RxOverflow) rather than silently dropping the tail and desyncing the stream.
    // Returns true iff the caller should re-post the request — never once dead, so a
    // dead or halted pipe is left alone and drains to zero requests in flight.
    bool onInComplete(bool ok, const uint8_t* data, size_t n) {
        if (dead()) return false;
        if (!ok) {
            fail(AaLinkFault::InError);
            return false;
        }
        if (n == 0) return true;  // zero-length packet: nothing to queue
        const size_t tail = tail_.load(std::memory_order_relaxed);  // producer-owned
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t used = (tail - head) & kMask;
        if (data == nullptr || n > kCapacity - used) {
            fail(AaLinkFault::RxOverflow);
            return false;
        }
        const size_t first = n < RingSize - tail ? n : RingSize - tail;
        std::memcpy(ring_ + tail, data, first);
        std::memcpy(ring_, data + first, n - first);
        tail_.store((tail + n) & kMask, std::memory_order_release);
        rxBytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed);
        return true;
    }

    // A bulk-IN request could not be posted (IRQ re-arm or task-context start).
    void onInRearmFailed() { fail(AaLinkFault::InRearmFailed); }

    // --- Consumer: task context ----------------------------------------------
    // Drain up to len buffered bytes: >0 = bytes copied, 0 = nothing buffered yet,
    // -1 = the link is dead (IAaTransport's "error/disconnect"; buffered bytes from
    // before the fault are discarded — the session they belong to is over).
    int read(uint8_t* buf, size_t len) {
        if (dead()) return -1;
        if (buf == nullptr || len == 0) return 0;
        const size_t head = head_.load(std::memory_order_relaxed);  // consumer-owned
        const size_t tail = tail_.load(std::memory_order_acquire);
        size_t n = (tail - head) & kMask;
        if (n > len) n = len;
        if (n > static_cast<size_t>(INT_MAX)) n = static_cast<size_t>(INT_MAX);
        const size_t first = n < RingSize - head ? n : RingSize - head;
        std::memcpy(buf, ring_ + head, first);
        std::memcpy(buf + first, ring_, n - first);
        head_.store((head + n) & kMask, std::memory_order_release);
        return static_cast<int>(n);
    }

    // Bulk-OUT bookkeeping (task context). write() must check writable() first and
    // never submit on a dead link.
    bool writable() const { return !dead(); }
    void onOutComplete(size_t n) {
        txBytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed);
    }
    void onOutFailed() { fail(AaLinkFault::OutError); }
    void onOutStalled() { fail(AaLinkFault::OutStalled); }

    // --- State (any context) --------------------------------------------------
    bool dead() const { return fault_.load(std::memory_order_acquire) != 0; }
    AaLinkFault fault() const {
        return static_cast<AaLinkFault>(fault_.load(std::memory_order_acquire));
    }
    size_t ringUsed() const {
        return (tail_.load(std::memory_order_acquire) -
                head_.load(std::memory_order_acquire)) & kMask;
    }
    uint32_t rxBytes() const { return rxBytes_.load(std::memory_order_relaxed); }
    uint32_t txBytes() const { return txBytes_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t kMask = RingSize - 1;

    // First fault wins (an IN error racing an OUT failure keeps whichever landed first).
    void fail(AaLinkFault f) {
        uint8_t expected = 0;
        fault_.compare_exchange_strong(expected, static_cast<uint8_t>(f),
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
    }

    uint8_t ring_[RingSize];
    std::atomic<size_t> head_{0};  // next byte the consumer reads
    std::atomic<size_t> tail_{0};  // next byte the producer writes
    std::atomic<uint8_t> fault_{0};
    std::atomic<uint32_t> rxBytes_{0};  // bytes accepted into the ring
    std::atomic<uint32_t> txBytes_{0};  // bytes the bulk-OUT pipe confirmed sent
};

}  // namespace carsh::aa

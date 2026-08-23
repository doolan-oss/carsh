// JitterBuffer — consumer-side prebuffer + underrun handling for a streaming
// PcmRing (the Android Auto media-audio path, AA_AUDIO_INVESTIGATION.md fix #2).
//
// The problem it solves: g_aaRing is fed in real time by the phone (~42 ms PCM
// frames) and drained in real time by the PWM DMA, so without a prebuffer the ring
// hovers near empty and ANY producer hiccup (a long H.264 decode stalling the
// refill, USB jitter) instantly underruns. Worse, the old mix path abandoned AA for
// the (silent) engine on a short read, turning each dip into an audible click.
//
// JitterBuffer wraps the ring's CONSUMER side and adds:
//   * a prime threshold — playback doesn't start (or resume) until at least
//     `primeElements` are buffered, so there's slack to ride out producer jitter;
//   * graceful underrun — once engaged, a short read emits SILENCE and keeps AA as
//     the active source (no source-switch click), then re-primes before resuming;
//   * disengage on a sustained empty (stream ended/disconnected), so other sources
//     (A2DP / local engine) take the output back, and the stale tail is discarded
//     so a reconnect doesn't splice old audio onto the new stream.
//
// Threading: every method that touches the ring or the state machine runs on the
// SOLE consumer (the PWM DMA/IRQ via BtMixSink::render). The producer only ever
// calls ring.write() from the AA task. The counters are atomics ONLY so the UI
// thread can read them for the serial heartbeat / on-screen meter — they are never
// written from two threads. This preserves the ring's SPSC contract: discard()
// (used on disengage) advances the consumer's tail index only, so it is safe here.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "audio/PcmRing.h"

namespace carsh::audio {

class JitterBuffer {
public:
    // ring: the SPSC ring this drains (its producer writes from elsewhere).
    // primeElements: occupancy (elements/samples, not frames) to buffer before the
    //   FIRST start (and a restart after a real pause) — the cushion against jitter.
    // resumeElements: smaller occupancy to resume after a TRANSIENT underrun while
    //   still engaged. Bursty wireless delivery (AA over a dongle) gaps for 200-600ms
    //   then clumps; requiring a full re-prime after each gap would add a prime-long
    //   silence on top of the gap. Resuming at a small threshold bounds the audible
    //   hit to ~the gap itself. 0 => same as primeElements (the original behaviour).
    JitterBuffer(PcmRing<float>& ring, std::size_t primeElements,
                 std::size_t resumeElements = 0)
        : ring_(ring),
          prime_(primeElements ? primeElements : 1),
          resume_(resumeElements ? resumeElements : prime_) {}

    // Consumer (PWM DMA/IRQ): try to fill `want` elements of `out` from the ring,
    // scaled by `gain`. Returns:
    //   true  -> AA owns this output block; `out[0..want)` is filled (real samples,
    //            or silence while priming/underrunning). The caller must NOT render
    //            another source over it.
    //   false -> AA is disengaged (nothing buffered to start, or the stream ended);
    //            `out` is left untouched so the caller can render A2DP / the engine.
    bool pull(float* out, std::size_t want, float gain) {
        const std::size_t avail = ring_.readable();

        // Not engaged: stay out of the mix until enough is buffered to play through
        // jitter. While accumulating, the caller's other sources keep playing.
        if (!engaged_.load(std::memory_order_relaxed)) {
            if (avail < prime_) return false;
            engaged_.store(true, std::memory_order_relaxed);
            primed_ = true;
            idle_ = 0;
            primeEvents_.fetch_add(1, std::memory_order_relaxed);
        }

        // Re-priming after a transient underrun: hold silence only until the ring
        // refills to the (small) resume threshold, then resume — so a brief delivery
        // gap costs ~the gap, not gap + a full re-prime. If it can't even reach
        // resume_ within the window the stream really stopped (a sub-`want` leftover
        // would otherwise sit forever), so disengage and hand the output back; the
        // next start then rebuilds the full prime_ cushion.
        if (!primed_) {
            if (avail >= resume_) {
                primed_ = true;
                idle_ = 0;
            } else {
                if (++idle_ >= kIdleBlocksToDisengage) {
                    disengage();          // stream really stopped -> release output
                    return false;
                }
                zeroFill(out, want);
                return true;              // AA still owns the output (silence, no click)
            }
        }

        // Primed and enough buffered: play through.
        if (avail >= want) {
            const std::size_t got = ring_.read(out, want);
            for (std::size_t i = 0; i < got; ++i) out[i] *= gain;
            for (std::size_t i = got; i < want; ++i) out[i] = 0.0f;  // defensive
            idle_ = 0;
            return true;
        }

        // Underrun while primed: don't drain the partial (keep it as the head of the
        // resumed stream for continuity); emit silence, count it, and require a
        // re-prime before playing again.
        underruns_.fetch_add(1, std::memory_order_relaxed);
        primed_ = false;
        zeroFill(out, want);
        return true;
    }

    // --- Diagnostics (any thread) ------------------------------------------
    std::uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t primeEvents() const { return primeEvents_.load(std::memory_order_relaxed); }
    bool engaged() const { return engaged_.load(std::memory_order_relaxed); }
    std::size_t occupancy() const { return ring_.readable(); }
    std::size_t primeElements() const { return prime_; }
    std::size_t resumeElements() const { return resume_; }

private:
    static void zeroFill(float* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) out[i] = 0.0f;
    }

    void disengage() {
        engaged_.store(false, std::memory_order_relaxed);
        primed_ = false;
        idle_ = 0;
        // Drop any stale tail so a later reconnect starts clean (consumer-side index
        // move only -> SPSC-safe). The producer has stopped by the time we get here.
        ring_.discard(ring_.readable());
    }

    PcmRing<float>& ring_;
    const std::size_t prime_;
    const std::size_t resume_;

    // Consumer-private state machine. primed_/idle_ are touched only by the consumer
    // (pull); engaged_ is atomic only so the meter/heartbeat can read it.
    std::atomic<bool> engaged_{false};
    bool primed_ = false;
    unsigned idle_ = 0;  // consecutive fully-empty blocks while engaged

    // ~0.5 s of fully-empty output blocks (at 1024-frame PWM chunks ≈ 21 ms each)
    // before we conclude the stream ended and hand the output back. Long enough to
    // ride out a real producer stall, short enough to release promptly on disconnect.
    static constexpr unsigned kIdleBlocksToDisengage = 24;

    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> primeEvents_{0};
};

}  // namespace carsh::audio

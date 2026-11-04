// CircleAaDecodeCore — runs the Android Auto H.264 decode on a dedicated A72 core via
// CMultiCoreSupport (AA_AUDIO_INVESTIGATION "candidate fix #1"). It is the bigger
// sibling of the cooperative CircleAaDecodeTask: identical pump body, but on its own
// core so software decode (~37 ms/frame, bench-measured > the 33 ms real-time budget)
// stops stealing core-0 time from the UI/audio loop — so the g_aaRing refill is never
// preempted by decode and AA audio stays smooth. (Video still can't beat real-time on
// one core; that's a separate throughput problem.)
//
// Requires ARM_ALLOW_MULTI_CORE in Circle's sysconfig.h (see
// scripts/patches/sysconfig-multicore.patch) — it spins up the secondary cores AND
// maps RAM inner-shareable cacheable so the cross-core SPSC handoffs are coherent.
// Gated in kernel.cpp behind CARSH_AA_DECODE_CORE (default on with openh264; build
// CARSH_NO_DECODE_CORE=1 to fall back to the single-core cooperative task).
//
// ── Cross-core data flow (all SPSC, lock-free, coherent-memory safe) ─────────
//   core 0 (AA poll):  AsyncH264Decoder::feed() -> AuQueue.push        [producer]
//   core 3 (this):     pump() -> AuQueue.pop -> openh264 decode -> onFrame ->
//                      kernel showFrame hook -> FrameRelay::publish    [producer]
//   core 0 (UI loop):  FrameRelay::acquire -> AndroidAutoScreen::setFrame/blit [consumer]
// AuQueue + FrameRelay use acquire/release exchanges; on the coherent A72 cluster
// (inner-shareable, from the multicore MMU) that is sufficient — no explicit cache
// maintenance. The audio path (g_aaRing) stays entirely on core 0. The auto-show
// flag is std::atomic<bool> (decode core sets, UI core consumes).
#ifndef _carsh_circle_aa_decode_core_h
#define _carsh_circle_aa_decode_core_h

#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/types.h>

#include "CircleAaDecodeTask.h"      // AaDecodeStats
#include "CircleClock.h"             // clockUs32(): decode timing
#include "aa/AsyncH264Decoder.h"

namespace carsh::device {

class CircleAaDecodeCore : public CMultiCoreSupport {
public:
    // The A72 core dedicated to decode. 0 = UI/audio/USB (the cooperative scheduler);
    // 3 keeps decode farthest from core 0's interrupt load. Cores 1-2 are parked.
    static constexpr unsigned kDecodeCore = 3;

    CircleAaDecodeCore(CMemorySystem* mem, carsh::aa::AsyncH264Decoder* dec,
                       AaDecodeStats* stats)
        : CMultiCoreSupport(mem), dec_(dec), stats_(stats) {}

    // Invoked on each secondary core (1..3) after Initialize(). Only kDecodeCore does
    // work; the others return so they park. The decode core spins the SAME pump body
    // as CircleAaDecodeTask — but here it has the core to itself, so it neither yields
    // nor steals time from the UI/audio loop on core 0.
    void Run(unsigned nCore) override {
        if (nCore != kDecodeCore) return;  // park cores 1,2 (and any core != decode)
        for (;;) {
            const uint32_t t0 = clockUs32();  // us; t1 - t0 is wrap-safe
            const unsigned n = dec_->pump(1);
            if (n > 0 && stats_ != nullptr)
                stats_->record(clockUs32() - t0);
            // No Yield() — this core is ours. A tiny pause keeps an empty queue from
            // hammering the bus; the AuQueue's acquire load still sees fresh AUs.
            if (n == 0) CTimer::SimpleusDelay(200);
        }
    }

private:
    carsh::aa::AsyncH264Decoder* dec_;
    AaDecodeStats* stats_;
};

}  // namespace carsh::device

#endif  // _carsh_circle_aa_decode_core_h

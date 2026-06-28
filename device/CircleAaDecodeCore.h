// CircleAaDecodeCore — SCAFFOLD (NOT WIRED): run the Android Auto H.264 decode on a
// dedicated A72 core via CMultiCoreSupport, the AA_AUDIO_INVESTIGATION "candidate fix
// #1" and the long-anticipated next step (scripts/fetch_openh264.sh: "Run feed() on a
// dedicated A72 core (CMultiCoreSupport)"). It is the bigger sibling of
// CircleAaDecodeTask: identical pump body, but on its own core so software decode
// stops stealing core-0 time from the UI/audio loop entirely — expected to fix BOTH
// the choppy audio (refill never preempted by decode) and the "slightly laggy video"
// (decode gets a full core for sustained throughput).
//
// ⚠️  THIS IS A SCAFFOLD. It compiles against Circle but is intentionally NOT
//     constructed in kernel.cpp. Bringing a second core up changes the whole app
//     from the proven single-core cooperative model to true SMP, and it can ONLY be
//     validated on the bench. Before wiring it, the items in "BENCH BRING-UP" below
//     MUST be done — otherwise the frame handoff races and the picture tears/faults.
//
// ── How it differs from the cooperative CircleAaDecodeTask ──────────────────
// The AuQueue (AsyncH264Decoder's input) is already a lock-free SPSC ring with
// acquire/release ordering, so producer-on-core-0 / consumer-on-core-3 is safe as-is.
// The OUTPUT is not: the cooperative task relied on "the decode task only runs while
// the UI task is yielded" so AndroidAutoScreen::setFrame (a borrowed pointer into the
// decoder's RGB scratch) was never read mid-write. On a real second core that
// guarantee is gone — core 0 can blit the RGB buffer while core 3 is rewriting it.
//
// ── BENCH BRING-UP checklist (do these before constructing this) ────────────
//  1. Double-buffer the decoded frame. Decode into one of two RGB buffers, then
//     publish its index with an atomic release store; the UI core loads it with an
//     acquire and blits that buffer. Never hand out a pointer into a buffer still
//     being written. (Put this in the frame sink, between CircleH264Decoder::emit
//     and AndroidAutoScreen::setFrame.)
//  2. Cache maintenance. The two cores share the RGB buffer through DRAM; after the
//     decode core finishes a frame, clean the data cache for that buffer before
//     publishing, and the UI core should read the freshly-published one. (Circle's
//     CleanDataCacheRange / the DMA-coherent allocator are the tools.)
//  3. Make the cross-core flags atomic. In kernel.cpp, aaAutoShowReq becomes
//     std::atomic<bool> (decode core sets, UI core consumes). AaDecodeStats is
//     already atomic. g_aaJitter/g_aaRing are unaffected (audio stays core-0).
//  4. Pick the core and park the rest. Run(nCore) is invoked on cores 1..3; do decode
//     only on kDecodeCore and return immediately on the others so they park.
//  5. Construct + Initialize() ONCE, after the UI objects exist, instead of
//     `new CircleAaDecodeTask(...)`. Keep the openh264 decoder, AsyncH264Decoder and
//     AaDecodeStats exactly as today — only the pump's home changes.
//
// ── Sketch of the kernel.cpp swap (inside the CARSH_HAVE_OPENH264 block) ─────
//     // instead of: new CircleAaDecodeTask(&aaAsync, &aaDecodeStats);
//     static CircleAaDecodeCore s_decodeCore(CMemorySystem::Get(), &aaAsync,
//                                            &aaDecodeStats);
//     if (!s_decodeCore.Initialize())
//         mLogger.Write(FROM, LogError, "AA: decode core init failed");
#ifndef _carsh_circle_aa_decode_core_h
#define _carsh_circle_aa_decode_core_h

#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/types.h>

#include "CircleAaDecodeTask.h"      // AaDecodeStats
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
            const unsigned t0 = CTimer::GetClockTicks();  // 1 MHz free-running (us)
            const unsigned n = dec_->pump(1);
            if (n > 0 && stats_ != nullptr)
                stats_->record(CTimer::GetClockTicks() - t0);
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

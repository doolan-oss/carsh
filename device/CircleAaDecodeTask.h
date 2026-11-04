// CircleAaDecodeTask — the cooperative pump that drains the AA video AuQueue and
// runs the (heavy) openh264 decode OFF the AA receive loop (AA_AUDIO_INVESTIGATION
// fix #3). Modelled on CircleDecodeTask / CircleBtRunLoopTask: each pass decodes at
// most one access unit, then Yield()s so the UI task can run tick() (which refills
// g_aaRing) and present the frame. Because the scheduler is cooperative on one core,
// this is the minimal, low-risk decoupling: a single keyframe decode no longer sits
// inside the same loop iteration that writes AA audio, and the g_aaRing prebuffer
// (JitterBuffer) covers the per-AU stall.
//
// It also times each decode and exposes max/avg (microseconds) so the kernel can
// log decode latency at ~1 Hz and correlate it with audio underruns (step 2 of the
// investigation). The dedicated-A72-core version (device/CircleAaDecodeCore.h)
// reuses the exact same pump body on a CMultiCoreSupport core.
#ifndef _carsh_circle_aa_decode_task_h
#define _carsh_circle_aa_decode_task_h

#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include <atomic>

#include "CircleClock.h"  // clockUs32(): decode timing
#include "aa/AsyncH264Decoder.h"

namespace carsh::device {

// Decode-latency telemetry, written by the pump and snapshotted by the UI task.
// Atomics so it stays correct if the pump later moves to a real second core.
struct AaDecodeStats {
    std::atomic<unsigned> lastUs{0};
    std::atomic<unsigned> maxUs{0};   // peak since the last snapshot()
    std::atomic<unsigned> sumUs{0};   // running sum since the last snapshot()
    std::atomic<unsigned> count{0};   // AUs decoded since the last snapshot()

    void record(unsigned us) {
        lastUs.store(us, std::memory_order_relaxed);
        unsigned m = maxUs.load(std::memory_order_relaxed);
        if (us > m) maxUs.store(us, std::memory_order_relaxed);
        sumUs.fetch_add(us, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
    }
    // Read max/avg/count over the window and reset the window. avgUs is 0 if idle.
    void snapshot(unsigned& maxOut, unsigned& avgOut, unsigned& countOut) {
        const unsigned c = count.exchange(0, std::memory_order_relaxed);
        const unsigned s = sumUs.exchange(0, std::memory_order_relaxed);
        maxOut = maxUs.exchange(0, std::memory_order_relaxed);
        countOut = c;
        avgOut = c ? s / c : 0;
    }
};

class CircleAaDecodeTask : public CTask {
public:
    CircleAaDecodeTask(carsh::aa::AsyncH264Decoder* dec, AaDecodeStats* stats)
        : dec_(dec), stats_(stats) {}

    void Run(void) override {
        for (;;) {
            const uint32_t t0 = clockUs32();  // us; t1 - t0 is wrap-safe
            const unsigned n = dec_->pump(1);             // one AU per pass, then yield
            if (n > 0 && stats_ != nullptr)
                stats_->record(clockUs32() - t0);
            CScheduler::Get()->Yield();
        }
    }

private:
    carsh::aa::AsyncH264Decoder* dec_;
    AaDecodeStats* stats_;
};

}  // namespace carsh::device

#endif  // _carsh_circle_aa_decode_task_h

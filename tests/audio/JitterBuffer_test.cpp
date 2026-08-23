// JitterBuffer — the AA media-audio prebuffer / underrun handler. These tests pin
// the consumer-side state machine that fixes the choppy-AA-audio root cause
// (AA_AUDIO_INVESTIGATION.md): don't start until primed, emit silence (not a
// source-switch) on a short read, re-prime before resuming, and disengage when the
// stream stops. The actual smoothness win is bench-only; this proves the logic.
#include <cmath>
#include <vector>

#include "Test.h"
#include "audio/JitterBuffer.h"
#include "audio/PcmRing.h"

using carsh::audio::JitterBuffer;
using carsh::audio::PcmRing;

namespace {
// Write `n` identical-valued elements into the ring (so reads are checkable).
void fill(PcmRing<float>& r, std::size_t n, float v) {
    std::vector<float> buf(n, v);
    r.write(buf.data(), n);
}
bool allEqual(const float* p, std::size_t n, float v) {
    for (std::size_t i = 0; i < n; ++i)
        if (std::fabs(p[i] - v) > 1e-6f) return false;
    return true;
}
}  // namespace

TEST(JitterBuffer, StaysDisengagedUntilPrimed) {
    PcmRing<float> ring(4096);
    JitterBuffer jb(ring, 1000);  // prime at 1000 elements
    float out[256];

    // Below prime -> disengaged, caller falls through (out untouched).
    fill(ring, 500, 0.5f);
    CHECK(!jb.pull(out, 256, 1.0f));
    CHECK(!jb.engaged());
    CHECK_EQ(jb.primeEvents(), std::uint64_t(0));

    // Cross the prime threshold -> engages and starts playing real samples.
    fill(ring, 600, 0.5f);  // now 1100 buffered
    CHECK(jb.pull(out, 256, 1.0f));
    CHECK(jb.engaged());
    CHECK_EQ(jb.primeEvents(), std::uint64_t(1));
    CHECK(allEqual(out, 256, 0.5f));
}

TEST(JitterBuffer, AppliesGain) {
    PcmRing<float> ring(4096);
    JitterBuffer jb(ring, 256);
    float out[256];
    fill(ring, 512, 1.0f);
    CHECK(jb.pull(out, 256, 0.25f));
    CHECK(allEqual(out, 256, 0.25f));
}

TEST(JitterBuffer, UnderrunEmitsSilenceAndKeepsOwningOutput) {
    PcmRing<float> ring(4096);
    JitterBuffer jb(ring, 512);
    float out[256];

    fill(ring, 600, 0.5f);            // prime
    CHECK(jb.pull(out, 256, 1.0f));   // engages, reads 256 (344 left)
    CHECK(jb.pull(out, 256, 1.0f));   // reads 256 -> 88 left, still >0 buffered

    // Next pull wants 256 but only 88 remain -> underrun: silence, STILL owns output
    // (returns true, not a fall-through to the engine), and counts the underrun.
    CHECK(jb.pull(out, 256, 1.0f));
    CHECK(allEqual(out, 256, 0.0f));
    CHECK_EQ(jb.underruns(), std::uint64_t(1));
    CHECK(jb.engaged());
}

TEST(JitterBuffer, RePrimesAfterUnderrunBeforeResuming) {
    PcmRing<float> ring(8192);
    JitterBuffer jb(ring, 1000);
    float out[256];

    fill(ring, 1024, 0.5f);                            // prime (multiple of 256)
    for (int i = 0; i < 4; ++i) CHECK(jb.pull(out, 256, 1.0f));  // drain to exactly empty
    CHECK_EQ(jb.occupancy(), std::size_t(0));
    CHECK(jb.pull(out, 256, 1.0f));                    // underrun -> silence, primed=false
    CHECK_EQ(jb.underruns(), std::uint64_t(1));

    // A trickle below the prime threshold must NOT resume playback (stays silent).
    fill(ring, 300, 0.9f);
    CHECK(jb.pull(out, 256, 1.0f));
    CHECK(allEqual(out, 256, 0.0f));  // still re-priming

    // Once it crosses prime again, playback resumes with the buffered samples (no
    // pre-underrun leftover here, so the resumed block is the new 0.9 data).
    fill(ring, 800, 0.9f);                             // 300 + 800 = 1100 >= prime
    CHECK(jb.pull(out, 256, 1.0f));
    CHECK(jb.engaged());
    CHECK(allEqual(out, 256, 0.9f));  // continuity: real audio, not silence
}

TEST(JitterBuffer, QuickResumeAfterTransientUnderrun) {
    // With a small resume threshold, a brief gap resumes as soon as ~a chunk is back
    // — it does NOT wait for the full prime cushion (the bursty-wireless fix).
    PcmRing<float> ring(8192);
    JitterBuffer jb(ring, /*prime*/ 1000, /*resume*/ 256);
    float out[256];
    fill(ring, 1024, 0.5f);
    for (int i = 0; i < 4; ++i) CHECK(jb.pull(out, 256, 1.0f));  // drain to empty
    CHECK(jb.pull(out, 256, 1.0f));                              // underrun -> silence
    CHECK_EQ(jb.underruns(), std::uint64_t(1));
    CHECK(jb.engaged());

    // Refill past resume(256) but well under prime(1000): must resume immediately.
    fill(ring, 300, 0.9f);
    CHECK(jb.pull(out, 256, 1.0f));
    CHECK(jb.engaged());
    CHECK(allEqual(out, 256, 0.9f));  // playing again, not still re-priming
}

TEST(JitterBuffer, DisengagesAfterSustainedEmptyAndDiscardsTail) {
    PcmRing<float> ring(8192);
    JitterBuffer jb(ring, 512);
    float out[256];

    fill(ring, 600, 0.5f);
    CHECK(jb.pull(out, 256, 1.0f));   // engage
    // Drain everything to empty, then keep pulling on an empty ring.
    CHECK(jb.pull(out, 256, 1.0f));   // 600 - 512 = 88 left
    CHECK(jb.pull(out, 256, 1.0f));   // underrun (88<256) -> silence, owns output

    // Keep pulling on a fully-empty ring; eventually it disengages (false) and the
    // caller gets the output back for A2DP / the engine.
    bool sawDisengage = false;
    for (int i = 0; i < 64; ++i) {
        if (!jb.pull(out, 256, 1.0f)) { sawDisengage = true; break; }
    }
    CHECK(sawDisengage);
    CHECK(!jb.engaged());

    // A late straggler that arrived below prime was discarded on disengage, so the
    // ring is clean for the next stream (no stale-audio splice on reconnect).
    fill(ring, 100, 0.7f);  // small, below prime
    // It's a fresh engage attempt: below prime so still disengaged, and only the
    // new 100 are present (not 100 + leftovers).
    CHECK(!jb.pull(out, 256, 1.0f));
    CHECK_EQ(jb.occupancy(), std::size_t(100));
}

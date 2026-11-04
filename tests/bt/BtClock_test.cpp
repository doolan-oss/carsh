// BtClock tests: BTstack's hal_time_ms() must be a full-range uint32 millisecond
// counter, because the run loop compares deadlines with (int32_t)(a - b). The old
// clock (32-bit microsecond ticks / 1000) jumped back to 0 every 71.6 min and
// stalled any pending timer for about 71 minutes (review finding B8).
#include "bt/BtClock.h"

#include <cstdint>

#include "Test.h"

using carsh::bt::btstackTimeDelta;
using carsh::bt::btstackTimeMs;

namespace {
constexpr uint64_t kUs32Wrap = uint64_t(1) << 32;  // 2^32 us = 71.58 min
// The old hal_time_ms(): CTimer::GetClockTicks() (32-bit us) / 1000.
uint32_t oldClockMs(uint64_t us) { return uint32_t(uint32_t(us) / 1000u); }
// BTstack's embedded run loop: set_timer(ms) -> deadline = now + ms + 1.
uint32_t armTimer(uint32_t nowMs, uint32_t ms) { return nowMs + ms + 1; }
}  // namespace

TEST(BtClock, IsContinuousAcrossThe32BitMicrosecondWrap) {
    CHECK_EQ(btstackTimeMs(kUs32Wrap - 1), uint32_t(4294967));
    CHECK_EQ(btstackTimeMs(kUs32Wrap), uint32_t(4294967));
    CHECK_EQ(btstackTimeMs(kUs32Wrap + 1000), uint32_t(4294968));
    // The old clock jumped back to 0 here.
    CHECK_EQ(oldClockMs(kUs32Wrap + 1000), uint32_t(1));
}

TEST(BtClock, TimerArmedJustBeforeThe71MinuteMarkFiresOnTime) {
    // The deferred AVRCP connect (1.5 s) is armed 0.5 s before 2^32 us of uptime.
    const uint64_t armedAt = kUs32Wrap - 500000;
    const uint64_t due = armedAt + 1502000;  // a little after the deadline

    const uint32_t deadline = armTimer(btstackTimeMs(armedAt), 1500);
    CHECK(btstackTimeDelta(deadline, btstackTimeMs(armedAt + 1000000)) > 0);  // not yet
    CHECK(btstackTimeDelta(deadline, btstackTimeMs(due)) <= 0);               // fires

    // With the old clock the same timer looked ~71.6 min away after the wrap.
    const uint32_t oldDeadline = armTimer(oldClockMs(armedAt), 1500);
    CHECK(btstackTimeDelta(oldDeadline, oldClockMs(due)) > 4000000);
}

TEST(BtClock, WrapsOnlyAtTheFullUint32MillisecondRange) {
    // ~49.7 days of uptime: the ms value wraps as a uint32, which BTstack's
    // (int32_t)(a - b) comparison handles.
    const uint64_t msWrapUs = (uint64_t(1) << 32) * 1000u;
    CHECK_EQ(btstackTimeMs(msWrapUs - 1000), uint32_t(0xFFFFFFFFu));
    CHECK_EQ(btstackTimeMs(msWrapUs), uint32_t(0));
    const uint32_t deadline = armTimer(btstackTimeMs(msWrapUs - 100000), 200);  // 100 ms before
    CHECK(btstackTimeDelta(deadline, btstackTimeMs(msWrapUs)) > 0);
    CHECK(btstackTimeDelta(deadline, btstackTimeMs(msWrapUs + 101000)) <= 0);
}

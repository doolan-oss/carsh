// CounterClock tests: the tick -> microsecond conversion behind carsh's monotonic
// clock must stay exact and monotonic past the point where Circle's
// CNTPCT * 1000000 / CNTFRQ overflows u64 (2^64 / 1e6 ticks, ~94.9 h at the Pi 4's
// 54 MHz), which froze AA input timestamps and jumped BTstack's clock back.
#include "hal/CounterClock.h"

#include <cstdint>

#include "Test.h"

using carsh::hal::counterToMicros;

namespace {
constexpr uint64_t kPi4Freq = 54000000;  // CNTFRQ_EL0 on the Pi 4 (armstub8.S)
constexpr uint64_t kPi3Freq = 19200000;  // and on the Pi 2/3
// First tick count at which Circle's 64-bit product count * 1e6 overflows.
constexpr uint64_t kCircleOverflowTicks = UINT64_MAX / 1000000u + 1;

// The exact answer, floor(count * 1e6 / freq), in 128-bit arithmetic.
uint64_t reference(uint64_t count, uint64_t freq) {
    return uint64_t((unsigned __int128)count * 1000000u / freq);
}
// Circle Step51's GetClockTicks64() on the Pi 4 (USE_PHYSICAL_COUNTER).
uint64_t circleClock(uint64_t count, uint64_t freq) { return count * 1000000u / freq; }
}  // namespace

TEST(CounterClock, MatchesTheExactConversionAtKnownPoints) {
    CHECK_EQ(counterToMicros(0, kPi4Freq), uint64_t(0));
    CHECK_EQ(counterToMicros(53, kPi4Freq), uint64_t(0));      // < 1 us: floor
    CHECK_EQ(counterToMicros(54, kPi4Freq), uint64_t(1));
    CHECK_EQ(counterToMicros(kPi4Freq, kPi4Freq), uint64_t(1000000));
    CHECK_EQ(counterToMicros(kPi4Freq * 3600, kPi4Freq), uint64_t(3600) * 1000000u);
    CHECK_EQ(counterToMicros(kPi3Freq + 96, kPi3Freq), uint64_t(1000005));
    CHECK_EQ(counterToMicros(1000, 0), uint64_t(0));  // unprogrammed counter
}

// The failure the review found: Circle's clock drops back near 0 at ~94.9 h of
// uptime; this one keeps counting, exactly.
TEST(CounterClock, StaysExactPastCirclesOverflowPoint) {
    const uint64_t hours = kCircleOverflowTicks / kPi4Freq / 3600;
    CHECK_EQ(hours, uint64_t(94));  // ~94.9 h at 54 MHz

    const uint64_t before = kCircleOverflowTicks - kPi4Freq;  // 1 s earlier
    const uint64_t after = kCircleOverflowTicks + kPi4Freq;   // 1 s later
    CHECK_EQ(counterToMicros(before, kPi4Freq), reference(before, kPi4Freq));
    CHECK_EQ(counterToMicros(after, kPi4Freq), reference(after, kPi4Freq));
    CHECK_EQ(counterToMicros(after, kPi4Freq) - counterToMicros(before, kPi4Freq),
             uint64_t(2000000));
    // Circle's formula went backwards across the same two seconds.
    CHECK(circleClock(after, kPi4Freq) < circleClock(before, kPi4Freq));

    // Far past it: 100 h, 1000 h, and the largest count the register can hold.
    for (uint64_t count : {kPi4Freq * 3600 * 100, kPi4Freq * 3600 * 1000,
                           kCircleOverflowTicks * 7 + 12345, UINT64_MAX - 1,
                           UINT64_MAX})
        CHECK_EQ(counterToMicros(count, kPi4Freq), reference(count, kPi4Freq));
    CHECK_EQ(counterToMicros(UINT64_MAX, kPi3Freq), reference(UINT64_MAX, kPi3Freq));
}

// Monotonic and exact tick by tick, including across the overflow point and on
// second boundaries (where the whole-second and remainder terms hand over).
TEST(CounterClock, IsMonotonicAndExactTickByTick) {
    for (uint64_t base : {uint64_t(0), kPi4Freq * 1000 - 500,
                          kCircleOverflowTicks - 500, UINT64_MAX - 2000}) {
        uint64_t prev = counterToMicros(base, kPi4Freq);
        for (uint64_t i = 1; i <= 1000; ++i) {
            const uint64_t count = base + i;
            const uint64_t us = counterToMicros(count, kPi4Freq);
            CHECK(us >= prev);
            CHECK(us - prev <= 1);  // 54 ticks per us: never skips a microsecond
            if (us != reference(count, kPi4Freq)) {
                CHECK_EQ(us, reference(count, kPi4Freq));
                break;
            }
            prev = us;
        }
    }
}

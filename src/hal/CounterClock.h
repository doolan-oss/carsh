// CounterClock — converts the ARM generic timer's tick count into microseconds
// without overflowing, for carsh's monotonic clock (device/CircleClock.h).
//
// Why carsh has its own: on the Pi 4, Circle Step51 always builds with
// USE_PHYSICAL_COUNTER, and CTimer::GetClockTicks64() returns
// CNTPCT * 1000000 / CNTFRQ computed in u64 (lib/timer.cpp). The product overflows
// once CNTPCT passes 2^64 / 1e6 ~= 1.84e13 ticks, which at the Pi 4's 54 MHz
// CNTFRQ (armstub8.S OSC_FREQ) is ~94.9 h of uptime. From then on the "64-bit"
// microsecond clock restarts near 0, and every carsh user of it broke with it: AA
// touch and key timestamps froze behind the kernel's monotonic clamp, BTstack's
// hal_time_ms jumped back, and the AA link supervisor's interval arithmetic
// wrapped. GetClockTicks() (32-bit) truncates the same overflowing product.
//
// Splitting the count into whole seconds and a remainder keeps every
// intermediate in range:
//   us = (count / freq) * 1e6 + (count % freq) * 1e6 / freq
// (count % freq) < freq, so the second product stays below freq * 1e6, and the
// first term is at most (2^64 / freq) * 1e6, which fits in 64 bits whenever
// freq >= 1 MHz. So for any freq from 1 MHz to ~1.8e13 Hz the result is exact
// (floor of count * 1e6 / freq) for EVERY 64-bit count: at 54 MHz it would take
// ~10,800 years for the count itself to wrap. Pure integer math, host-tested
// (tests/hal/CounterClock_test).
#pragma once

#include <cstdint>

namespace carsh::hal {

// Microseconds represented by `count` ticks of a counter running at `freqHz`,
// rounded down. 0 when freqHz is 0 (a counter that was never programmed).
constexpr uint64_t counterToMicros(uint64_t count, uint64_t freqHz) {
    if (freqHz == 0) return 0;
    return (count / freqHz) * 1000000u + (count % freqHz) * 1000000u / freqHz;
}

}  // namespace carsh::hal

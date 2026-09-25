// CircleClock — carsh's monotonic microsecond clock on the Pi 4, for every
// timestamp and interval carsh measures (AA touch/key stamps, the media session's
// stream timing, BTstack's hal_time_ms, the AA link supervisor, diagnostics).
//
// It reads the ARM generic timer directly, exactly as Circle does on the Pi 4, but
// converts with hal::counterToMicros() instead of Circle's
// CNTPCT * 1000000 / CNTFRQ, whose u64 product overflows after ~94.9 h of uptime
// (see src/hal/CounterClock.h). Checked against Circle Step51: timer.cpp #errors
// unless USE_PHYSICAL_COUNTER is set on RASPPI >= 4, and GetClockTicks64() then
// reads these same two registers at EL1. EL1 access is enabled before Circle drops
// from EL2 (startup64.S sets CNTHCTL_EL2.EL1PCTEN/EL1PCEN, bits 0-1), and the
// armstub programs CNTFRQ_EL0 = 54 MHz (armstub8.S OSC_FREQ). The counter is
// system-wide, so the value is the same on every core.
//
// Bare-metal aarch64 only (it is inline asm); device code includes it, host code
// never does.
#ifndef _carsh_circle_clock_h
#define _carsh_circle_clock_h

#include <cstdint>

#include "hal/CounterClock.h"

namespace carsh::device {

// Microseconds since the counter started (power-on). Monotonic for the lifetime of
// the board: exact for any counter value (hal::counterToMicros), no ~95 h jump.
// Safe from any context (task, IRQ, any core): two register reads and a division.
inline uint64_t clockUs() {
    uint64_t count, freq;
    // The ISB keeps the counter read from being hoisted above earlier
    // instructions, as Circle's own GetClockTicks64() does.
    asm volatile("isb" ::: "memory");
    asm volatile("mrs %0, cntpct_el0" : "=r"(count));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return carsh::hal::counterToMicros(count, freq);
}

// The low 32 bits, for short interval measurements (t1 - t0 in unsigned
// arithmetic is exact across the 2^32 us wrap, ~71.6 min, for intervals under it).
inline uint32_t clockUs32() { return static_cast<uint32_t>(clockUs()); }

}  // namespace carsh::device

#endif

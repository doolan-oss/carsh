// HalCircle.cpp — the tiny BTstack embedded HAL backed by Circle, used only when
// the image is built with CARSH_HAVE_BTSTACK=1. The embedded run loop
// (btstack_run_loop_embedded.c, selected via HAVE_EMBEDDED_TIME_MS in
// btstack_config.h) needs just a millisecond clock and IRQ enable/disable; there
// is no periodic tick handler in this configuration.
//
// BTstack is C and calls these as C symbols, so they are declared extern "C".
#ifdef CARSH_HAVE_BTSTACK

#include <circle/synchronize.h>

#include "CircleClock.h"  // clockUs(): carsh's overflow-free microsecond clock
#include "bt/BtClock.h"   // btstackTimeMs(): the wrap contract, host-tested

extern "C" {
#include "hal_time_ms.h"
#include "hal_cpu.h"
}

// Milliseconds since boot, as a full-range uint32 that wraps after 2^32 ms
// (~49.7 days). BTstack's run loop compares deadlines with wrap-safe 32-bit
// subtraction and relies on that. It must come from a continuous 64-bit
// microsecond count: CTimer::GetClockTicks() is a 32-bit microsecond count that
// wraps every 71.6 min, and dividing it by 1000 made this clock jump back to 0 at
// that point, so timers pending across the jump fired about 71 min late (review
// B8). Circle's GetClockTicks64() is no better after ~94.9 h (its u64 product
// overflows and it restarts near 0), hence clockUs().
extern "C" uint32_t hal_time_ms(void) {
    return carsh::bt::btstackTimeMs(carsh::device::clockUs());
}

// BTstack guards run-loop data-structure updates against its (transport) IRQ
// callbacks with these. Map to Circle's nestable critical sections; the
// disable/enable calls are always balanced 1:1 in the embedded run loop.
extern "C" void hal_cpu_disable_irqs(void) { EnterCritical(); }
extern "C" void hal_cpu_enable_irqs(void)  { LeaveCritical(); }

// In this configuration the run loop is pumped via execute_once() from a
// cooperative CTask, so we must NOT actually sleep here (that would stall the
// Circle scheduler and starve audio/UI). Just re-enable IRQs and return; the
// task's own Yield() provides the cooperative break.
extern "C" void hal_cpu_enable_irqs_and_sleep(void) { LeaveCritical(); }

#endif  // CARSH_HAVE_BTSTACK

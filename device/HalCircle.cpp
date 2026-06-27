// HalCircle.cpp — the tiny BTstack embedded HAL backed by Circle, used only when
// the image is built with CARSH_HAVE_BTSTACK=1. The embedded run loop
// (btstack_run_loop_embedded.c, selected via HAVE_EMBEDDED_TIME_MS in
// btstack_config.h) needs just a millisecond clock and IRQ enable/disable; there
// is no periodic tick handler in this configuration.
//
// BTstack is C and calls these as C symbols, so they are declared extern "C".
#ifdef CARSH_HAVE_BTSTACK

#include <circle/timer.h>
#include <circle/synchronize.h>

extern "C" {
#include "hal_time_ms.h"
#include "hal_cpu.h"
}

// Milliseconds since boot. CTimer::GetClockTicks() is the free-running 1 MHz
// (CLOCKHZ) system counter, so /1000 yields ms. Wraps after ~49 days, which is
// exactly the uint32_t millisecond contract BTstack's run loop expects (it does
// timeout math in wrap-safe 32-bit arithmetic).
extern "C" uint32_t hal_time_ms(void) {
    return CTimer::GetClockTicks() / (CLOCKHZ / 1000);
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

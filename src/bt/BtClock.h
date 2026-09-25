// BtClock — the millisecond clock behind BTstack's embedded run loop
// (hal_time_ms() in device/HalCircle.cpp), kept here so the wrap behaviour is
// host-tested (tests/bt/BtClock_test).
//
// BTstack compares timer deadlines with btstack_time_delta(a, b) =
// (int32_t)(a - b) (btstack_util.c). That is wrap-safe only if the clock is a
// full-range uint32 millisecond counter, which wraps after 2^32 ms (~49.7 days).
// A 32-bit MICROsecond counter divided by 1000 does not qualify: it jumps from
// 4294967 back to 0 every 2^32 us (71.6 min). Any timer pending across that jump
// then sees a delta of about +4.29e6 ms and fires about 71 minutes late (review
// finding B8). That covers L2CAP RTX, the A2DP set-config timer and the glue's
// deferred AVRCP connect. So the clock must come from a 64-bit microsecond count.
#pragma once

#include <cstdint>

namespace carsh::bt {

// Milliseconds since boot, in BTstack's hal_time_ms() format, from a 64-bit
// microsecond count (e.g. Circle's CTimer::GetClockTicks64()). Truncating to
// 32 bits gives exactly the full-range uint32 wrap that BTstack assumes.
constexpr uint32_t btstackTimeMs(uint64_t micros) {
    return static_cast<uint32_t>(micros / 1000u);
}

// BTstack's btstack_time_delta(): positive while `deadline` is still ahead of
// `now`. A run-loop timer fires once this is <= 0.
constexpr int32_t btstackTimeDelta(uint32_t deadline, uint32_t now) {
    return static_cast<int32_t>(deadline - now);
}

}  // namespace carsh::bt

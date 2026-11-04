// Touch input abstraction (PLAN.md §2.3).
//
// HW backend: Circle CRPiTouchScreen, which reads the firmware touchbuffer
// published via the GET_TOUCHBUF mailbox tag (no direct I2C — R2 resolved).
// Host backend: SyntheticTouch, which replays scripted event sequences so UI
// hit-testing and gestures are testable without a digitizer.
//
// The driver layer emits raw down/move/up points, each stamped with its capture
// time (on the device, CircleTouch pumps the firmware touch buffer at 100 Hz from
// the timer IRQ into a hal::TouchRing); the gesture layer (GestureRecognizer)
// turns them into taps and vertical swipes for list flinging.
#pragma once

#include <cstdint>

namespace carsh::hal {

enum class TouchPhase : std::uint8_t { Down = 0, Move = 1, Up = 2 };

struct TouchEvent {
    TouchPhase phase = TouchPhase::Down;
    int id = 0;          // contact id (multi-touch); single-touch uses 0
    int x = 0;           // panel coords, 0..799
    int y = 0;           // panel coords, 0..479
    std::uint32_t tMs = 0;  // monotonic timestamp, ms
};

class ITouch {
public:
    virtual ~ITouch() = default;

    // Dequeues the next pending event into out. Returns false if none pending.
    virtual bool poll(TouchEvent& out) = 0;
};

}  // namespace carsh::hal

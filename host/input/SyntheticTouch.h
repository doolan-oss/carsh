// SyntheticTouch — host backend for ITouch (PLAN.md working method).
//
// Replays a scripted sequence of touch events so UI hit-testing and gestures are
// testable without the digitizer. Helpers build common interactions (tap, swipe,
// drag) as event streams matching what CRPiTouchScreen + the gesture layer would
// emit; `id` is the contact id, for multi-touch scripts. For the driver's
// snapshot semantics (intermediate states lost between reads), see SnapshotTouch.
#pragma once

#include <deque>

#include "hal/Touch.h"

namespace carsh::host {

class SyntheticTouch : public carsh::hal::ITouch {
public:
    bool poll(carsh::hal::TouchEvent& out) override {
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

    void push(const carsh::hal::TouchEvent& e) { queue_.push_back(e); }
    bool empty() const { return queue_.empty(); }
    void clear() { queue_.clear(); }

    // A single tap at (x,y): down then up. Real CRPiTouchScreen reports FingerUp
    // with no coordinates (0,0), so we mirror that — the gesture layer must derive
    // the release point from the last Down/Move, not the Up event.
    void scriptTap(int x, int y, std::uint32_t tMs = 0, int id = 0) {
        push({carsh::hal::TouchPhase::Down, id, x, y, tMs});
        push({carsh::hal::TouchPhase::Up, id, 0, 0, tMs + 30});
    }

    // A vertical swipe from (x,y0) to (x,y1) over `steps` move events. The final
    // Move lands on (x,y1); the Up carries no coordinates (0,0), as on hardware.
    void scriptSwipe(int x, int y0, int y1, int steps = 8,
                     std::uint32_t tMs = 0, std::uint32_t stepMs = 10, int id = 0) {
        scriptDrag(x, y0, x, y1, steps, tMs, stepMs, id);
    }

    // A straight 2-D drag from (x0,y0) to (x1,y1) over `steps` move events, then
    // an Up at (0,0).
    void scriptDrag(int x0, int y0, int x1, int y1, int steps = 8,
                    std::uint32_t tMs = 0, std::uint32_t stepMs = 10, int id = 0) {
        push({carsh::hal::TouchPhase::Down, id, x0, y0, tMs});
        for (int i = 1; i <= steps; ++i) {
            const int x = x0 + (x1 - x0) * i / steps;
            const int y = y0 + (y1 - y0) * i / steps;
            push({carsh::hal::TouchPhase::Move, id, x, y,
                  tMs + std::uint32_t(i) * stepMs});
        }
        push({carsh::hal::TouchPhase::Up, id, 0, 0,
              tMs + std::uint32_t(steps + 1) * stepMs});
    }

private:
    std::deque<carsh::hal::TouchEvent> queue_;
};

}  // namespace carsh::host

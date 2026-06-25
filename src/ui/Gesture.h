// GestureRecognizer — turns raw touch events into taps and vertical swipes
// (PLAN.md §2.3). Down records the start; Up within a small radius is a tap;
// larger vertical motion is a swipe (carrying total dy for list flinging).
#pragma once

#include <cstdlib>

#include "hal/Touch.h"

namespace carsh::ui {

enum class GestureType { None, Tap, Swipe };

struct Gesture {
    GestureType type = GestureType::None;
    int x = 0, y = 0;   // tap point, or swipe end point
    int dy = 0;         // total vertical travel (swipe; +down, -up)
    int dx = 0;
};

class GestureRecognizer {
public:
    // Tap if travel stays within this radius; otherwise a swipe.
    explicit GestureRecognizer(int tapRadius = 12) : tapRadius_(tapRadius) {}

    // Feed a raw event. Returns true and fills `out` when a gesture completes
    // (on Up).
    bool feed(const carsh::hal::TouchEvent& e, Gesture& out) {
        using P = carsh::hal::TouchPhase;
        if (e.phase == P::Down) {
            sx_ = e.x; sy_ = e.y; lastX_ = e.x; lastY_ = e.y; down_ = true;
            return false;
        }
        if (e.phase == P::Move) {
            lastX_ = e.x; lastY_ = e.y;
            return false;
        }
        // Up
        if (!down_) return false;
        down_ = false;
        int dx = e.x - sx_, dy = e.y - sy_;
        out.dx = dx; out.dy = dy; out.x = e.x; out.y = e.y;
        if (std::abs(dx) <= tapRadius_ && std::abs(dy) <= tapRadius_)
            out.type = GestureType::Tap;
        else
            out.type = GestureType::Swipe;
        return true;
    }

private:
    int tapRadius_;
    int sx_ = 0, sy_ = 0, lastX_ = 0, lastY_ = 0;
    bool down_ = false;
};

}  // namespace carsh::ui

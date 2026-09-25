// GestureRecognizer — turns raw touch events into taps and vertical swipes
// (PLAN.md §2.3). Down records the start; Up within a small radius is a tap;
// larger vertical motion is a swipe (carrying total dy for list flinging).
//
// Single contact: the Down that starts a gesture latches its contact id, and
// events from any other id are ignored until that contact's Up. The Pi panel
// reports up to 10 contacts, and without the latch a second finger (a palm on
// the bezel, a pinch) would reset the start point, mix its moves into the
// release point, and end the gesture with its own Up.
#pragma once

#include <cstdlib>

#include "hal/Touch.h"

namespace carsh::ui {

enum class GestureType { None, Tap, Swipe };

struct Gesture {
    GestureType type = GestureType::None;
    int x = 0, y = 0;   // tap point, or swipe end point
    int sx = 0, sy = 0; // start point (the Down); swipes hit-test here
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
            // A second contact while one is tracked is ignored. A Down on the
            // tracked id itself (the driver never sends one: a re-touch between
            // reads shows up as a Move) means its Up was lost, so start over.
            if (down_ && e.id != id_) return false;
            id_ = e.id;
            sx_ = e.x; sy_ = e.y; lastX_ = e.x; lastY_ = e.y; down_ = true;
            return false;
        }
        if (!down_ || e.id != id_) return false;
        if (e.phase == P::Move) {
            lastX_ = e.x; lastY_ = e.y;
            return false;
        }
        // Up. The release point comes from the last Down/Move, NOT the Up event:
        // the Pi touch driver (CRPiTouchScreen) reports FingerUp at (0,0), so
        // trusting e.x/e.y here misclassifies every tap as a long swipe and routes
        // it to (0,0). lastX_/lastY_ track the true finger position.
        down_ = false;
        int dx = lastX_ - sx_, dy = lastY_ - sy_;
        out.dx = dx; out.dy = dy; out.x = lastX_; out.y = lastY_;
        out.sx = sx_; out.sy = sy_;
        if (std::abs(dx) <= tapRadius_ && std::abs(dy) <= tapRadius_)
            out.type = GestureType::Tap;
        else
            out.type = GestureType::Swipe;
        return true;
    }

    // Abandon the gesture in progress (if any): its remaining Moves and Up
    // complete nothing. Used when the screen it started on goes away mid-touch.
    void cancel() { down_ = false; }

private:
    int tapRadius_;
    int sx_ = 0, sy_ = 0, lastX_ = 0, lastY_ = 0;
    int id_ = 0;
    bool down_ = false;
};

}  // namespace carsh::ui

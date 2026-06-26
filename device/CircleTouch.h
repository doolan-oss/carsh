// CircleTouch — device backend for hal::ITouch over Circle's CRPiTouchScreen
// (PLAN.md §2.3). The firmware publishes touch via the GET_TOUCHBUF mailbox
// (R2 resolved); CRPiTouchScreen reads it. Its event handler is a plain function
// pointer with no user param, so we route through a static self-pointer into a
// queue that poll() drains. Call update() >60 Hz from the UI/input loop.
#ifndef _carsh_circle_touch_h
#define _carsh_circle_touch_h

#include <circle/devicenameservice.h>
#include <circle/input/rpitouchscreen.h>
#include <circle/input/touchscreen.h>

#include <deque>

#include "hal/Touch.h"

namespace carsh::device {

class CircleTouch : public carsh::hal::ITouch {
public:
    bool initialize() {
        if (!mTouch.Initialize()) return false;
        mDevice = static_cast<CTouchScreenDevice*>(
            CDeviceNameService::Get()->GetDevice("touch1", FALSE));
        if (!mDevice) return false;
        s_self = this;
        mDevice->RegisterEventHandler(&CircleTouch::handler);
        return true;
    }

    // CRPiTouchScreen self-updates via a periodic timer registered in
    // Initialize() (it polls the firmware touchbuffer ~60 Hz and fires the event
    // handler), so there is nothing to pump here — just drain the queue.
    bool poll(carsh::hal::TouchEvent& out) override {
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

private:
    static void handler(TTouchScreenEvent Event, unsigned nID, unsigned nPosX,
                        unsigned nPosY) {
        if (!s_self) return;
        carsh::hal::TouchEvent e;
        e.id = int(nID);
        e.x = int(nPosX);
        e.y = int(nPosY);
        switch (Event) {
            case TouchScreenEventFingerDown: e.phase = carsh::hal::TouchPhase::Down; break;
            case TouchScreenEventFingerUp:   e.phase = carsh::hal::TouchPhase::Up;   break;
            case TouchScreenEventFingerMove: e.phase = carsh::hal::TouchPhase::Move; break;
            default: return;
        }
        s_self->queue_.push_back(e);
    }

    CRPiTouchScreen mTouch;
    CTouchScreenDevice* mDevice = nullptr;
    std::deque<carsh::hal::TouchEvent> queue_;
    static CircleTouch* s_self;
};

// Defined in kernel.cpp (single TU) to keep the static linkage simple.
// inline avoids a separate definition requirement.
inline CircleTouch* CircleTouch::s_self = nullptr;

}  // namespace carsh::device

#endif

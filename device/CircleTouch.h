// CircleTouch — device backend for hal::ITouch over Circle's CRPiTouchScreen
// (PLAN.md §2.3). The firmware publishes touch via the GET_TOUCHBUF mailbox
// (R2 resolved); CRPiTouchScreen reads it. Its event handler is a plain function
// pointer with no user param, so we route through a static self-pointer into a
// queue that poll() drains.
//
// Driving the driver: Circle does NOT self-pump this. CTouchScreenDevice::Update()
// is what reads the firmware touch buffer and fires events, and its header says
// "Call this about 60 times per second!" — nothing in CRPiTouchScreen registers a
// timer to do that. So poll() pumps Update() itself. Also, CTouchScreenDevice
// requires Setup(CDisplay*) before any event can be reported (ReportHandler
// dereferences the display for coordinate mapping), so initialize() takes the
// framebuffer and wires it in.
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
    // display: the framebuffer the UI renders to (CBcmFrameBuffer is a CDisplay).
    // CTouchScreenDevice::Setup() needs it for coordinate mapping; without it,
    // the first reported event null-derefs the display in ReportHandler.
    bool initialize(CDisplay* display) {
        if (!mTouch.Initialize()) return false;
        mDevice = static_cast<CTouchScreenDevice*>(
            CDeviceNameService::Get()->GetDevice("touch1", FALSE));
        if (!mDevice) return false;
        mDevice->Setup(display);  // REQUIRED before Update()/event reporting
        s_self = this;
        mDevice->RegisterEventHandler(&CircleTouch::handler);
        return true;
    }

    // Pump the driver, then drain. CTouchScreenDevice::Update() polls the firmware
    // touch buffer and synchronously fires our handler (which enqueues), so one
    // Update() when the queue is empty refills it for this frame. The UI loop
    // calls poll() until it returns false, so Update() runs ~once per frame.
    bool poll(carsh::hal::TouchEvent& out) override {
        if (queue_.empty()) mDevice->Update();
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

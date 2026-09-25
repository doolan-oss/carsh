// CircleTouch — device backend for hal::ITouch over Circle's CRPiTouchScreen
// (PLAN.md §2.3). The firmware publishes touch via the GET_TOUCHBUF mailbox
// (R2 resolved); CRPiTouchScreen reads it. Its event handler is a plain function
// pointer with no user param, so we route through a static self-pointer into a
// queue that poll() drains.
//
// Driving the driver: Circle does NOT self-pump this. CTouchScreenDevice::Update()
// is what reads the firmware touch buffer and fires events, and its header says
// "Call this about 60 times per second!" — nothing in CRPiTouchScreen registers a
// timer to do that. The firmware keeps ONE snapshot and Update() only diffs the
// latest one against the last it saw, so a tap or the middle of a flick that falls
// between two Update() calls is lost. Pumping it from the UI loop (one call per
// rendered frame, 45-100 ms apart) lost short taps and turned flicks into taps, so
// Update() now runs from Circle's periodic timer handler: HZ = 100 times a second,
// in the timer IRQ on core 0, independent of rendering. That path is IRQ-safe:
// Update() is plain reads of the coherent touch page + the CDisplay size/offset
// getters + our handler, which only stamps the event and pushes it into a
// fixed-size lock-free SPSC ring (hal::TouchRing — no allocation, no locks, no
// logging). poll(), on the UI task, only drains the ring.
//
// Also, CTouchScreenDevice requires Setup(CDisplay*) before any event can be
// reported (ReportHandler dereferences the display for coordinate mapping), so
// initialize() takes the framebuffer and wires it in.
#ifndef _carsh_circle_touch_h
#define _carsh_circle_touch_h

#include <circle/devicenameservice.h>
#include <circle/input/rpitouchscreen.h>
#include <circle/input/touchscreen.h>
#include <circle/timer.h>

#include <atomic>
#include <cstdint>

#include "hal/Touch.h"
#include "hal/TouchRing.h"

namespace carsh::device {

class CircleTouch : public carsh::hal::ITouch {
public:
    CircleTouch() = default;
    CircleTouch(const CircleTouch&) = delete;
    CircleTouch& operator=(const CircleTouch&) = delete;

    // Circle has no way to unregister a periodic handler, so it outlives this
    // object: clearing the self-pointer turns the handler into a no-op. (The IRQ
    // and this destructor run on the same core, so no handler is mid-flight here.)
    ~CircleTouch() { s_self.store(nullptr, std::memory_order_release); }

    // display: the framebuffer the UI renders to (CBcmFrameBuffer is a CDisplay).
    // CTouchScreenDevice::Setup() needs it for coordinate mapping; without it,
    // the first reported event null-derefs the display in ReportHandler.
    // timer: the kernel's CTimer, whose 100 Hz tick pumps the driver.
    bool initialize(CDisplay* display, CTimer* timer) {
        if (!mTouch.Initialize()) return false;
        mDevice = static_cast<CTouchScreenDevice*>(
            CDeviceNameService::Get()->GetDevice("touch1", FALSE));
        if (!mDevice) return false;
        mDevice->Setup(display);  // REQUIRED before Update()/event reporting
        mDevice->RegisterEventHandler(&CircleTouch::handler);
        // Publish self last: from here on the timer IRQ may pump Update().
        s_self.store(this, std::memory_order_release);
        timer->RegisterPeriodicHandler(&CircleTouch::periodic);
        return true;
    }

    // Drain one event (UI task). Never touches the driver — the timer IRQ pumps it.
    bool poll(carsh::hal::TouchEvent& out) override {
        carsh::hal::StampedTouch s;
        if (!ring_.pop(s)) return false;
        out = s.ev;
        lastUs_ = s.us;
        return true;
    }

    // Capture time (64-bit µs, CTimer::GetClockTicks64) of the event the last
    // successful poll() returned — the AA input channel's event timestamp.
    uint64_t lastEventUs() const { return lastUs_; }

    // Events the ring refused under backlog (diagnostics).
    uint32_t dropped() const { return ring_.dropped(); }

private:
    // Timer IRQ, 100 Hz, core 0.
    static void periodic(void) {
        CircleTouch* self = s_self.load(std::memory_order_acquire);
        if (self != nullptr && self->mDevice != nullptr) self->mDevice->Update();
    }

    // Called synchronously from Update(), i.e. in the timer IRQ.
    static void handler(TTouchScreenEvent Event, unsigned nID, unsigned nPosX,
                        unsigned nPosY) {
        CircleTouch* self = s_self.load(std::memory_order_acquire);
        if (self == nullptr) return;
        carsh::hal::StampedTouch s;
        s.us = CTimer::GetClockTicks64();
        s.ev.id = int(nID);
        s.ev.x = int(nPosX);
        s.ev.y = int(nPosY);
        s.ev.tMs = static_cast<uint32_t>(s.us / 1000u);
        switch (Event) {
            case TouchScreenEventFingerDown: s.ev.phase = carsh::hal::TouchPhase::Down; break;
            case TouchScreenEventFingerUp:   s.ev.phase = carsh::hal::TouchPhase::Up;   break;
            case TouchScreenEventFingerMove: s.ev.phase = carsh::hal::TouchPhase::Move; break;
            default: return;
        }
        self->ring_.pushEvent(s);
    }

    CRPiTouchScreen mTouch;
    CTouchScreenDevice* mDevice = nullptr;
    carsh::hal::TouchRing ring_;  // producer: timer IRQ; consumer: poll() (UI task)
    uint64_t lastUs_ = 0;         // UI task only
    // The handlers are plain function pointers (no user param); read in the IRQ.
    static inline std::atomic<CircleTouch*> s_self{nullptr};
};

}  // namespace carsh::device

#endif

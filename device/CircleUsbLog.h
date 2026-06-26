// CircleUsbLog — exposes the firmware log over the Pi 4 USB-C port as a USB CDC
// ACM serial gadget. When a host connects (it appears as e.g. /dev/ttyACM0 on a
// Linux PC), the logger is retargeted to it, so the board narrates over the very
// USB-C cable that powers it — no GPIO serial adapter needed.
//
// Runs as a cooperative CTask: USB enumeration needs the plug-and-play state
// machine serviced far more often than the ~30 fps UI loop, and the scheduler
// runs this task repeatedly during the UI loop's MsSleep(). The USB-C port is the
// dwc2 OTG controller, independent of the xHCI host that drives the USB-A music
// stick (CUSBHCIDevice) — the two coexist on the Pi 4.
#ifndef _carsh_circle_usb_log_h
#define _carsh_circle_usb_log_h

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/usb/gadget/usbcdcgadget.h>
#include <circle/usb/usbserial.h>

namespace carsh::device {

class CircleUsbLog : public CTask {
public:
    // gadget: the CDC gadget (already Initialize()d). logger: retargeted to the
    // USB serial on connect. fallback: the target restored on disconnect (UART).
    CircleUsbLog(CUSBCDCGadget* gadget, CLogger* logger, CDevice* fallback)
        : mGadget(gadget), mLogger(logger), mFallback(fallback) {}

    void Run(void) override {
        for (;;) {
            if (mGadget->UpdatePlugAndPlay() && !mSerial) {
                mSerial = static_cast<CUSBSerialDevice*>(
                    CDeviceNameService::Get()->GetDevice("utty1", FALSE));
                if (mSerial) {
                    mSerial->RegisterRemovedHandler(removed, this);
                    mSerial->SetOptions(SERIAL_OPTION_ONLCR);  // \n -> \r\n for terminals
                    mLogger->SetNewTarget(mSerial);
                    mLogger->Write("usblog", LogNotice, "USB-C serial log attached");
                }
            }
            // Drain host->device bytes so the CDC RX path never backs up.
            if (mSerial) {
                char buf[64];
                mSerial->Read(buf, sizeof buf);
            }
            CScheduler::Get()->Yield();
        }
    }

private:
    static void removed(CDevice*, void* ctx) {
        auto* self = static_cast<CircleUsbLog*>(ctx);
        self->mSerial = nullptr;
        self->mLogger->SetNewTarget(self->mFallback);  // back to the UART
    }

    CUSBCDCGadget* mGadget;
    CLogger* mLogger;
    CDevice* mFallback;
    CUSBSerialDevice* volatile mSerial = nullptr;
};

}  // namespace carsh::device

#endif

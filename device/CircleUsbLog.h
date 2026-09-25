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

#include "ota/PushProtocol.h"

namespace carsh::device {

class CircleUsbLog : public CTask {
public:
    // gadget: the CDC gadget (already Initialize()d). logger: retargeted to the
    // USB serial on connect. fallback: the target restored on disconnect (UART).
    // push: optional sink for host->device bytes (the serial OTA push channel);
    // without it, inbound bytes are drained and discarded as before.
    CircleUsbLog(CUSBCDCGadget* gadget, CLogger* logger, CDevice* fallback,
                 carsh::ota::PushReceiver* push = nullptr)
        : mGadget(gadget), mLogger(logger), mFallback(fallback), mPush(push) {}

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
            // Drain host->device bytes so the CDC RX path never backs up. With a
            // push receiver attached they carry the serial OTA transfer; loop-
            // drain (bounded per pass) so a multi-MB push moves at USB speed,
            // not one small buffer per scheduler round.
            if (mSerial) {
                uint8_t buf[512];
                for (unsigned pass = 0; pass < 32; ++pass) {
                    const int n = mSerial->Read(buf, sizeof buf);
                    if (n <= 0) break;
                    if (mPush) mPush->feed(buf, static_cast<std::size_t>(n));
                }
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
    carsh::ota::PushReceiver* mPush;
    CUSBSerialDevice* volatile mSerial = nullptr;
};

}  // namespace carsh::device

#endif

// carsh device kernel (PLAN.md §2.1). Derives from CStdlibApp (base: interrupts,
// exception handler, ActLED, options) and adds serial logging + a timer, so the
// log goes to the UART (works under QEMU and over a USB-UART/ESP32 bridge) while
// the UI owns the framebuffer. This is the Step-2 bring-up kernel; subsystem
// bring-up (FAT, audio, touch) is layered on in subsequent milestones.
#ifndef _carsh_kernel_h
#define _carsh_kernel_h

#include <circle_stdlib_app.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/logger.h>

class CKernel : public CStdlibApp {
public:
    CKernel(void);

    bool Initialize(void) override;
    TShutdownMode Run(void) override;

private:
    CSerialDevice mSerial;
    CTimer mTimer;
    CLogger mLogger;
};

#endif

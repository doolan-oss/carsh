// carsh device kernel (PLAN.md §2.1). Derives from CStdlibApp (base: interrupts,
// exception handler, ActLED, options) and adds serial logging, a timer, the SD
// card (EMMC) + FatFs mount, and the framebuffer. The log goes to the UART
// (QEMU + USB-UART/ESP32) so the UI can own the framebuffer.
//
// Step-2 bring-up: mounts the SD, scans it into the library via the same Vfs/
// indexer/decoder core the host tests cover, and renders the UI. Audio/touch/
// flash/BT backends are layered on in subsequent milestones.
#ifndef _carsh_kernel_h
#define _carsh_kernel_h

#include <circle_stdlib_app.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/usb/usbhcidevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/sched/scheduler.h>

#include <deque>
#include <string>

#include "hal/Vfs.h"  // carsh::hal::Source (scanLibrary parameter)

class CKernel : public CStdlibApp {
public:
    CKernel(void);

    bool Initialize(void) override;
    TShutdownMode Run(void) override;

private:
    void mountStorage(void);                       // SD (EMMC) + USB mass storage
    void scanLibrary(carsh::hal::Source& src);     // index a source into the Database
    void checkOta(void);      // verify a staged USB OTA package (no auto-flash)

    // Mirror a formatted line to both the UART log and the on-screen diagnostics
    // panel (mDiag), so storage/USB bring-up is visible without a serial cable.
    void diag(const char* fmt, ...);

    CSerialDevice mSerial;
    CTimer mTimer;
    CLogger mLogger;
    CEMMCDevice mEMMC;
    CUSBHCIDevice mUSBHCI;    // USB host controller (enables USB mass storage)
    CScheduler mScheduler;   // cooperative scheduler for the decode/UI tasks
    FATFS mFileSystem;       // SD: volume
    FATFS mUsbFs;            // USB: volume
    bool mSdMounted = false;
    bool mUsbMounted = false;
    std::deque<std::string> mDiag;  // recent storage/USB diagnostic lines
    bool mDiagVisible = true;       // on-screen diagnostics panel (toggle: tap top-right)
};

#endif

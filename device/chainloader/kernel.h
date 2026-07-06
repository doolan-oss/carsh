// carsh A/B chainloader (PLAN.md §3 Phase 5) — the tiny kernel installed as the
// SD card's boot image. Each boot it reads SD:/boot_slot.bin, runs the
// host-tested try/rollback decision (ota::decide), persists the consumed try,
// loads the chosen slot image (SD:/kernel8-A.img or -B.img) into memory, and
// warm-jumps to it via Circle's chainboot. The app image never boots directly:
// firmware -> chainloader -> slot image, which is what makes a bad OTA push
// recoverable (tries exhaust -> rollback to the other slot).
//
// Logs to the GPIO UART only: the USB-C CDC console belongs to the app; the
// chainloader must add near-zero boot latency and zero USB state.
#ifndef _carsh_chainloader_kernel_h
#define _carsh_chainloader_kernel_h

#include <circle_stdlib_app.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

class CKernel : public CStdlibApp {
public:
    CKernel(void);

    bool Initialize(void) override;
    TShutdownMode Run(void) override;

private:
    CSerialDevice mSerial;
    CTimer mTimer;
    CLogger mLogger;
    CEMMCDevice mEMMC;
    FATFS mFileSystem;
    bool mSdMounted = false;
};

#endif

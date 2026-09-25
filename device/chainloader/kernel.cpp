// carsh A/B chainloader — see kernel.h. Decision logic is src/ota/BootSlots.h
// (host-tested); this file is only the FatFs + chainboot plumbing around it.
#include "kernel.h"

#include <circle/chainboot.h>
#include <circle/logger.h>
#include <circle/sysconfig.h>

#include "ota/BootSlots.h"

// Review finding O1. carsh builds libcircle with ARM_ALLOW_MULTI_CORE
// (scripts/patches/sysconfig-multicore.patch, for the AA decode core), and stock
// Circle's EnableChainBoot() is `assert (0)` in that configuration: this kernel
// would halt on every boot and a migrated card would never reach a slot image.
// scripts/patches/chainboot-multicore.patch allows chain boot while the
// secondary cores are still parked in the armstub spin table — always the case
// here, since the chainloader never creates a CMultiCoreSupport, so the chained
// app releases them exactly as on a cold boot. Refuse to build against a
// multi-core Circle without that patch instead of shipping a chainloader that
// can't chain; Run() also calls IsChainBootAllowed(), which only exists in a
// libcircle rebuilt with the patch, so a patched header over a stale
// libcircle.a fails to link rather than asserting on the Pi.
#if defined(ARM_ALLOW_MULTI_CORE) && !defined(CHAINBOOT_MULTICORE_SAFE)
#error "multi-core libcircle without scripts/patches/chainboot-multicore.patch: EnableChainBoot() would assert(0) on every boot. Apply it and rebuild libcircle (scripts/patches/README.md)."
#endif

#define FROM "chain"

namespace {

constexpr const char* kRecordPath = "SD:/boot_slot.bin";

const char* slotImagePath(uint8_t slot) {
    return slot == 0 ? "SD:/kernel8-A.img" : "SD:/kernel8-B.img";
}

bool fileExists(const char* path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

bool loadRecord(carsh::ota::BootRecord& r) {
    FIL fil;
    if (f_open(&fil, kRecordPath, FA_READ) != FR_OK) return false;
    uint8_t buf[carsh::ota::kBootRecordLen];
    UINT br = 0;
    const FRESULT res = f_read(&fil, buf, sizeof buf, &br);
    f_close(&fil);
    return res == FR_OK && br == sizeof buf &&
           carsh::ota::deserializeBootRecord(buf, br, r);
}

bool saveRecord(const carsh::ota::BootRecord& r) {
    uint8_t buf[carsh::ota::kBootRecordLen];
    carsh::ota::serializeBootRecord(r, buf);
    FIL fil;
    if (f_open(&fil, kRecordPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    UINT bw = 0;
    const FRESULT res = f_write(&fil, buf, sizeof buf, &bw);
    f_sync(&fil);
    f_close(&fil);
    return res == FR_OK && bw == sizeof buf;
}

}  // namespace

CKernel::CKernel(void)
    : CStdlibApp("carsh-chain"),
      mSerial(&mInterrupt),
      mTimer(&mInterrupt),
      mLogger(mOptions.GetLogLevel(), &mTimer),
      mEMMC(&mInterrupt, &mTimer, &mActLED) {}

bool CKernel::Initialize(void) {
    if (!CStdlibApp::Initialize()) return false;
    if (!mSerial.Initialize(115200)) return false;
    if (!mLogger.Initialize(&mSerial)) return false;
    if (mEMMC.Initialize() && f_mount(&mFileSystem, "SD:", 1) == FR_OK)
        mSdMounted = true;
    return true;
}

CStdlibApp::TShutdownMode CKernel::Run(void) {
    if (!mSdMounted) {
        mLogger.Write(FROM, LogPanic, "SD mount failed — cannot boot a slot");
        return ShutdownHalt;
    }

    // Read the A/B record; a fresh/pre-OTA card has none — bootstrap to slot A
    // (make_sdcard installs the app image as kernel8-A.img).
    carsh::ota::BootRecord rec;
    if (!loadRecord(rec)) {
        mLogger.Write(FROM, LogWarning, "no %s — bootstrapping to slot A",
                      kRecordPath);
        rec = {};
        rec.activeSlot = 0;
        rec.healthy = true;
        saveRecord(rec);  // best-effort; decide() below works either way
    }

    // Try/rollback decision (host-tested). Persist the consumed try BEFORE the
    // jump: if the trial image wedges, the next power-cycle sees one fewer try
    // and eventually rolls back — that's the whole safety property.
    const carsh::ota::BootDecision d = carsh::ota::decide(rec);
    if (d.rolledBack)
        mLogger.Write(FROM, LogError,
                      "slot %c trial exhausted — ROLLED BACK to slot %c",
                      'A' + carsh::ota::otherSlot(d.bootSlot), 'A' + d.bootSlot);
    if (!saveRecord(d.next))
        mLogger.Write(FROM, LogError, "cannot persist %s", kRecordPath);

    // Pick the slot image; fall back to the other slot if the file is missing
    // (e.g. bootstrap card where only kernel8-A.img exists).
    uint8_t slot = d.bootSlot;
    if (!fileExists(slotImagePath(slot))) {
        mLogger.Write(FROM, LogWarning, "%s missing — trying slot %c",
                      slotImagePath(slot), 'A' + carsh::ota::otherSlot(slot));
        slot = carsh::ota::otherSlot(slot);
    }

    FIL fil;
    if (f_open(&fil, slotImagePath(slot), FA_READ) != FR_OK) {
        mLogger.Write(FROM, LogPanic, "no bootable slot image (A or B)");
        return ShutdownHalt;
    }
    const FSIZE_t size = f_size(&fil);
    uint8_t* image = new uint8_t[size];
    UINT br = 0;
    const FRESULT res = f_read(&fil, image, UINT(size), &br);
    f_close(&fil);
    if (res != FR_OK || br != size) {
        mLogger.Write(FROM, LogPanic, "slot %c read failed (%u/%u bytes)",
                      'A' + slot, unsigned(br), unsigned(size));
        return ShutdownHalt;
    }

    mLogger.Write(FROM, LogNotice, "chainboot slot %c v%u (%u bytes)%s",
                  'A' + slot,
                  slot == 0 ? d.next.versionA : d.next.versionB,
                  unsigned(size), d.next.trying ? " [trial]" : "");

#ifdef CHAINBOOT_MULTICORE_SAFE
    // Always true here (no secondary core was ever started); checked so a
    // regression is a clear panic on the GPIO UART, not a bare assert.
    if (!IsChainBootAllowed()) {
        mLogger.Write(FROM, LogPanic, "chain boot not allowed (secondary cores up)");
        return ShutdownHalt;
    }
#endif

    // Hand over: Circle's shutdown path sees the pending chainboot and jumps to
    // the loaded image instead of resetting (main() must RETURN, not reboot()).
    EnableChainBoot(image, size);
    return ShutdownReboot;
}

// CircleFlashTarget — device backend for ota::IFlashTarget (PLAN.md §3 Phase 5).
// Writes the inactive slot's kernel image to the FAT boot partition via FatFs.
// The chainloader (a separate tiny kernel8.img) reads boot_slot.bin and jumps to
// kernel8-A/B.img; this just stages the image. Forced-bad-boot rollback is
// validated at the bench (HARDWARE_TODO H7). Uses only FIL (no DIR collision).
#ifndef _carsh_circle_flash_target_h
#define _carsh_circle_flash_target_h

#include <fatfs/ff.h>

#include <string>

#include "ota/IFlashTarget.h"

namespace carsh::device {

class CircleFlashTarget : public carsh::ota::IFlashTarget {
public:
    // drive: FatFs boot volume (e.g. "SD:"). Slot images are kernel8-A/B.img.
    explicit CircleFlashTarget(std::string drive = "SD:")
        : drive_(std::move(drive)) {}

    bool writeSlotImage(uint8_t slot, const uint8_t* image,
                        std::size_t len) override {
        std::string path = drive_ + (slot == 0 ? "/kernel8-A.img"
                                               : "/kernel8-B.img");
        FIL fil;
        if (f_open(&fil, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
            return false;
        UINT bw = 0;
        FRESULT r = f_write(&fil, image, UINT(len), &bw);
        f_sync(&fil);
        f_close(&fil);
        return r == FR_OK && bw == len;
    }

private:
    std::string drive_;
};

}  // namespace carsh::device

#endif

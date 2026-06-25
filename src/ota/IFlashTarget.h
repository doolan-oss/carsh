// IFlashTarget — writes a kernel image into a boot slot (PLAN.md §3 Phase 5).
//
// This is the one genuinely hardware-gated step of OTA: on the Pi it writes the
// inactive kernel8-<slot>.img on the FAT boot partition. The host backend
// (MemFlashTarget) records writes in RAM so the verify -> stage -> A/B logic is
// fully testable; the real SD write + reboot are validated at the bench (H7).
#pragma once

#include <cstddef>
#include <cstdint>

namespace carsh::ota {

class IFlashTarget {
public:
    virtual ~IFlashTarget() = default;
    // Write the image for the given slot (0 = A, 1 = B). Returns false on error.
    virtual bool writeSlotImage(uint8_t slot, const uint8_t* image,
                                std::size_t len) = 0;
};

}  // namespace carsh::ota

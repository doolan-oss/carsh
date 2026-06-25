// MemFlashTarget — host IFlashTarget that records slot writes in RAM, so OTA
// apply/verify/stage logic is testable without touching real boot media (H7).
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "ota/IFlashTarget.h"

namespace carsh::host {

class MemFlashTarget : public carsh::ota::IFlashTarget {
public:
    bool writeSlotImage(uint8_t slot, const uint8_t* image,
                        std::size_t len) override {
        if (failNext_) { failNext_ = false; return false; }
        slots_[slot].assign(image, image + len);
        return true;
    }
    const std::vector<uint8_t>& slot(uint8_t s) { return slots_[s]; }
    bool hasSlot(uint8_t s) const { return slots_.count(s) != 0; }
    void failNextWrite() { failNext_ = true; }

private:
    std::map<uint8_t, std::vector<uint8_t>> slots_;
    bool failNext_ = false;
};

}  // namespace carsh::host

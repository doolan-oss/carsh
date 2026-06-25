// Theme — terminal-aesthetic color palette (PLAN.md §2.2). Accent is #d97757.
// Variants map to the per-profile ThemeVariant. Colors are 0xAARRGGBB.
#pragma once

#include <cstdint>

#include "hal/IRenderTarget.h"

namespace carsh::ui {

struct Theme {
    uint32_t bg;       // background
    uint32_t panel;    // panel/list background
    uint32_t dim;      // dim text / inactive
    uint32_t bright;   // bright text / active
    uint32_t accent;   // accent (#d97757 default)
    uint32_t accentBg; // accent fill behind selection

    static Theme dark() {
        return {carsh::hal::rgba(12, 12, 14), carsh::hal::rgba(22, 22, 26),
                carsh::hal::rgba(120, 120, 130), carsh::hal::rgba(230, 230, 235),
                carsh::hal::rgba(0xD9, 0x77, 0x57),
                carsh::hal::rgba(0x3a, 0x24, 0x1c)};
    }
    static Theme light() {
        return {carsh::hal::rgba(240, 240, 242), carsh::hal::rgba(224, 224, 228),
                carsh::hal::rgba(110, 110, 120), carsh::hal::rgba(20, 20, 24),
                carsh::hal::rgba(0xC0, 0x5a, 0x3a),
                carsh::hal::rgba(0xf0, 0xd8, 0xcc)};
    }
    static Theme amber() {
        return {carsh::hal::rgba(10, 8, 4), carsh::hal::rgba(26, 20, 8),
                carsh::hal::rgba(150, 110, 40), carsh::hal::rgba(255, 200, 90),
                carsh::hal::rgba(0xFF, 0xB0, 0x40),
                carsh::hal::rgba(0x40, 0x2a, 0x08)};
    }
};

}  // namespace carsh::ui

#include "ui/Font.h"

#include "font/font8x8_basic.h"  // char font8x8_basic[128][8] (public domain)

namespace carsh::ui {

uint8_t glyphRow(char ch, int row) {
    unsigned char c = (unsigned char)ch;
    if (c >= 128 || row < 0 || row >= 8) return 0;
    return uint8_t(font8x8_basic[c][row]);
}

}  // namespace carsh::ui

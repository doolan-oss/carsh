// Monospace 8x8 font access for the grid toolkit (PLAN.md §2.2). On the Pi the
// plan bakes a TTF (JetBrains/IBM Plex Mono) to an atlas offline; for the host
// build and tests we use the public-domain font8x8 atlas so text renders
// deterministically. Same accessor shape either way.
#pragma once

#include <cstdint>

namespace carsh::ui {

inline constexpr int kCellW = 8;
inline constexpr int kCellH = 8;

// Returns the bitmap row (0..7) for character ch; bit c (LSB-first) is the
// leftmost pixel of column c. Non-printable/out-of-range -> blank row.
uint8_t glyphRow(char ch, int row);

}  // namespace carsh::ui

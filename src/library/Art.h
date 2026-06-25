// Cover-art + ASCII generator (PLAN.md §2.10, §4.5).
//
// Decodes embedded/sidecar JPEG/PNG (stb_image), box-downscales to a small cell
// grid, maps luminance to a density ramp, and extracts a dominant tint color.
// The derived AsciiArt is cached on disk (rows/cols/cells + tint) so generation
// is rare. Pure host-testable image math; the only platform dependency is the
// byte source (Vfs), which is injected.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carsh::library {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4, row-major
    bool valid() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * h * 4; }
};

struct AsciiArt {
    int rows = 0, cols = 0;
    std::string cells;          // rows*cols chars from the density ramp
    uint8_t tintR = 0, tintG = 0, tintB = 0;

    // .ascii cache blob: ["ASC1"][u16 rows][u16 cols][3 tint][cells].
    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, std::size_t len, AsciiArt& out);
    char at(int r, int c) const { return cells[size_t(r) * cols + c]; }
};

// Decode JPEG/PNG/BMP bytes into RGBA. Returns false on failure.
bool decodeImage(const uint8_t* data, std::size_t len, Image& out);

// Box-filter downscale to dw x dh.
Image downscale(const Image& src, int dw, int dh);

// 0xAARRGGBB dominant tint (saturation-weighted average).
uint32_t dominantColor(const Image& img);

// Generate ASCII art at cols x rows (cells aspect-corrected for ~2:1 glyphs).
AsciiArt generateAscii(const Image& img, int cols = 22, int rows = 11);

// The density ramp, dark -> bright.
extern const char* kDensityRamp;

}  // namespace carsh::library

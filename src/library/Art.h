// Cover-art + ASCII generator (PLAN.md §2.10, §4.5).
//
// Decodes embedded/sidecar JPEG/PNG (stb_image), box-downscales to a small cell
// grid, maps luminance to a density ramp, and extracts a dominant tint color.
// The derived AsciiArt is cached on disk (rows/cols/cells + tint) so generation
// is rare. Pure host-testable image math; the only platform dependency is the
// byte source (Vfs), which is injected.
//
// Memory: the device path is decodeImageFitted(), which streams the file through
// a 64 KiB window (never buffers it whole), rejects covers above kMaxArtDim from
// the header alone, and box-downscales straight out of stb's pixel buffer to a
// kArtWorkDim working image — the only full-size allocation is stb's own, freed
// before return. Circle's stock heap loses every freed block > 512 KiB, and the
// old read-file + decode + copy path cost ~10 MiB per 1000x1000 cover (review L1).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hal/Vfs.h"

namespace carsh::library {

// Largest cover side we decode. stb_image cannot scale while decoding, so the
// transient cost is the full pixel buffer plus JPEG component planes: ~40-55 MB
// at 3000x3000 (the common store / "max size" download), which the large heap
// buckets (scripts/patches/sysconfig-heap-buckets.patch) then keep reusable.
// Anything bigger (raw scans) is rejected; ArtSource falls back to the next
// image in the folder.
constexpr int kMaxArtDim = 3000;

// Working size decodeImageFitted() downscales to: far more than the 22x11 ASCII
// grid and the tint need, small enough to stay in a 256 KiB heap bucket.
constexpr int kArtWorkDim = 256;

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

// Decode JPEG/PNG/BMP bytes into full-resolution RGBA. Returns false on failure
// or when either side exceeds maxDim (checked from the header, before decoding).
// Materialises the whole image (plus stb's buffer while copying): for small
// images and tests — the device path is decodeImageFitted().
bool decodeImage(const uint8_t* data, std::size_t len, Image& out,
                 int maxDim = kMaxArtDim);

// Decode a JPEG/PNG/BMP stream into RGBA scaled to fit workDim x workDim (aspect
// kept; smaller images keep their size). Reads the stream through a bounded
// window, rejects images with a side > maxDim before decoding, and downscales
// directly from the decoder's buffer. Returns false on failure / rejection.
bool decodeImageFitted(carsh::hal::IStream& s, Image& out,
                       int workDim = kArtWorkDim, int maxDim = kMaxArtDim);

// Box-filter downscale to dw x dh.
Image downscale(const Image& src, int dw, int dh);

// 0xAARRGGBB dominant tint (saturation-weighted average).
uint32_t dominantColor(const Image& img);

// Generate ASCII art at cols x rows (cells aspect-corrected for ~2:1 glyphs).
AsciiArt generateAscii(const Image& img, int cols = 22, int rows = 11);

// The density ramp, dark -> bright.
extern const char* kDensityRamp;

}  // namespace carsh::library

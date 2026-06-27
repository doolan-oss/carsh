// Yuv420 — I420 (planar YUV 4:2:0) to packed RGB888 conversion for the Android
// Auto video path. The H.264 decoder emits I420; the UI surface
// (AndroidAutoScreen) blits RGB888, so the decoder backend converts here. BT.601
// limited-range coefficients (the colour space AA video uses), pure integer math so
// the result is identical on host and device — the device backend swaps in a NEON
// kernel with the same coefficients. Host-tested (tests/aa/H264Decode_test).
#pragma once

#include <cstddef>
#include <cstdint>

namespace carsh::aa {

inline uint8_t clampU8(int x) {
    return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
}

// Convert an I420 frame to row-major packed RGB888 (R,G,B per pixel). y/u/v point at
// the three planes with the given byte strides (chroma planes are width/2 x
// height/2). `out` must hold width*height*3 bytes. width/height are treated as even.
inline void i420ToRgb888(const uint8_t* y, int strideY, const uint8_t* u, int strideU,
                         const uint8_t* v, int strideV, int width, int height,
                         uint8_t* out) {
    for (int j = 0; j < height; ++j) {
        const uint8_t* yr = y + static_cast<size_t>(j) * strideY;
        const uint8_t* ur = u + static_cast<size_t>(j >> 1) * strideU;
        const uint8_t* vr = v + static_cast<size_t>(j >> 1) * strideV;
        uint8_t* orow = out + static_cast<size_t>(j) * width * 3;
        for (int i = 0; i < width; ++i) {
            const int c = static_cast<int>(yr[i]) - 16;
            const int d = static_cast<int>(ur[i >> 1]) - 128;
            const int e = static_cast<int>(vr[i >> 1]) - 128;
            orow[i * 3 + 0] = clampU8((298 * c + 409 * e + 128) >> 8);
            orow[i * 3 + 1] = clampU8((298 * c - 100 * d - 208 * e + 128) >> 8);
            orow[i * 3 + 2] = clampU8((298 * c + 516 * d + 128) >> 8);
        }
    }
}

}  // namespace carsh::aa
